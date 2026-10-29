#include "Nametags.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"
#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dice::hfe::nametags {

namespace {

float clamp01(float v)
{
	return std::clamp(v, 0.0f, 1.0f);
}

const std::string* setting(const world::ObjectTemplate* t, const char* name)
{
	if (!t) {
		return nullptr;
	}
	const auto it = t->settings().find(name);
	return it == t->settings().end() ? nullptr : &it->second;
}

float number(const world::ObjectTemplate* t, const char* name, float fallback)
{
	const std::string* value = setting(t, name);
	return value ? static_cast<float>(std::atof(value->c_str())) : fallback;
}

// ColorArray (Shaders/Nametag) for the player and object tags: the
// NametagManager's constructor (0x100f24c0) fills the table at 0x102112f8
// that tag types 0 and 3 point at: white, red, blue, green, yellow.
const float kColours[5][4] = {
	{1, 1, 1, 1},
	{0.87f, 0.094f, 0.094f, 1},
	{0.254f, 0.552f, 0.89f, 1},
	{74 / 255.0f, 227 / 255.0f, 37 / 255.0f, 1},
	{1, 1, 0, 1},
};

struct Vec {
	float x = 0, y = 0, z = 0;
};
Vec operator-(const Vec& a, const Vec& b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}
float dot(const Vec& a, const Vec& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec forwardOf(const Nametags::View& view)
{
	return {std::sin(view.yaw) * std::cos(view.pitch), std::sin(view.pitch), std::cos(view.yaw) * std::cos(view.pitch)};
}

} // namespace

void Fade::in(float seconds, float delay, float time)
{
	if (alpha <= 0.01f && timer < delay) {
		timer += seconds;
		return;
	}
	alpha = std::min(1.0f, alpha + (time > 0 ? seconds / time : 1.0f));
}

void Fade::out(float seconds, float time)
{
	alpha = std::max(0.0f, alpha - (time > 0 ? seconds / time : 1.0f));
	timer = 0;
}

// 0x100f0e60 (the local player alive; the spectator's branch is left out).
float playerTagAlpha(const Settings& s, const PlayerTagInput& in, Fade& fade, float seconds)
{
	if (in.sameVehicle || (!in.friendly && in.manDown)) {
		fade.out(seconds, s.deathFadeOutTime);
		return 0;
	}
	if (!in.alive) {
		fade.out(seconds, s.deathFadeOutTime);
		return fade.alpha;
	}
	if (in.manDown) {
		fade.out(seconds, in.localMedic ? s.manDownFadeOutTime : s.deathFadeOutTime);
		return fade.alpha;
	}
	const float d = in.distance;
	if (in.friendly) {
		if (in.aimedAt) {
			const float most = s.aimedFriendlyDistance;
			fade.in(seconds, 0, 0.7f);
			return fade.alpha * clamp01(1 - (d - most * 2 / 3) / (most / 3));
		}
		fade.out(seconds, 1.5f);
		if (d < s.maxFriendlyDistance) {
			// Full to 80 m, gone at 120 (maxFriendlyDistance - 40 .. it).
			fade.alpha = std::max(fade.alpha, clamp01(1 - (d - (s.maxFriendlyDistance - 40)) / 40));
		}
		return fade.alpha;
	}
	// An enemy: only while aimed at, faded in over its distance's share of
	// the cull range.
	const float most = s.playerCullFar;
	if (d < most && in.aimedAt) {
		fade.in(seconds, d / most, d / most);
	} else {
		fade.out(seconds, 1.0f);
	}
	// The distance over 0.95, the cull range's last third and the fog
	// (0x100f09b0) cap the alpha.
	const float far = d * 1.0526316f;
	return std::min(fade.alpha, tagFog(s.fog, far) * clamp01(1 - (far - most * 2 / 3) / (most / 3)));
}

float tagFog(const std::array<float, 4>& f, float distance)
{
	const float start = f[0], end = f[1];
	if (end <= start) {
		return 1;
	}
	const float r0 = 1 / (end - start), r1 = -f[2] * r0, r2 = -start * r0, r3 = end * r0;
	const float far = distance * r0 + r2;
	return clamp01(std::max(r3, distance * r1 + r3) - far * far * far);
}

Colour playerTagColour(const PlayerTagInput& in)
{
	if (!in.friendly) {
		return Enemy;
	}
	return in.sameSquad ? Squad : Team;
}

int shownDistance(int shown, float distance)
{
	const float step = distance < 25 ? 0 : distance < 250 ? 2 : distance < 400 ? 5 : 9;
	const int rounded = static_cast<int>(std::lround(distance));
	if (shown < 0 || std::abs(rounded - shown) > step) {
		return rounded;
	}
	return shown;
}

bool TagFont::parse(const std::string& text)
{
	std::istringstream in(text);
	std::string line;
	bool glyphs = false;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (!glyphs) {
			const size_t eq = line.find('=');
			if (line.rfind("---", 0) == 0) {
				glyphs = true;
				continue;
			}
			if (eq == std::string::npos) {
				continue;
			}
			std::string key = line.substr(0, eq);
			key.erase(key.find_last_not_of(' ') + 1);
			std::string value = line.substr(eq + 1);
			value.erase(0, value.find_first_not_of(' '));
			if (key == "Texture") {
				texture = value + ".dds";
			} else if (key == "TextureWidth") {
				textureWidth = std::atoi(value.c_str());
			} else if (key == "TextureHeight") {
				textureHeight = std::atoi(value.c_str());
			} else if (key == "Height") {
				height = std::atoi(value.c_str());
			} else if (key == "BetweenWidth") {
				between = std::atoi(value.c_str());
			}
			continue;
		}
		if (line.size() < 3) {
			continue;
		}
		const unsigned char c = static_cast<unsigned char>(line[0]);
		Cell cell;
		if (std::sscanf(line.c_str() + 2, "%d %d %d", &cell.x0, &cell.y, &cell.x1) == 3) {
			cells[c] = cell;
			present[c] = true;
		}
	}
	return glyphs && !texture.empty();
}

float TagFont::width(const std::string& text) const
{
	float w = 0;
	for (unsigned char c : text) {
		if (present[c]) {
			w += static_cast<float>(cells[c].x1 - cells[c].x0 + 1 + between);
		}
	}
	return w;
}

void TagFont::write(const std::string& text, float x, float y, const float colour[4], std::vector<hud::HudQuad>& out) const
{
	for (unsigned char c : text) {
		if (!present[c]) {
			continue;
		}
		const Cell& cell = cells[c];
		const float w = static_cast<float>(cell.x1 - cell.x0 + 1);
		if (c == ' ') {
			x += w + static_cast<float>(between); // its cell is only room
			continue;
		}
		hud::HudQuad q;
		q.texture = texture;
		q.x0 = x;
		q.y0 = y;
		q.x1 = x + w;
		q.y1 = y + static_cast<float>(height);
		q.u0 = static_cast<float>(cell.x0) / static_cast<float>(textureWidth);
		q.u1 = static_cast<float>(cell.x1 + 1) / static_cast<float>(textureWidth);
		q.v0 = static_cast<float>(cell.y) / static_cast<float>(textureHeight);
		q.v1 = static_cast<float>(cell.y + height) / static_cast<float>(textureHeight);
		std::copy(colour, colour + 4, q.color);
		out.push_back(q);
		x += w + static_cast<float>(between);
	}
}

int orderIconId(bool fromCommander, int kind)
{
	// 0x100f4620: 0x39 + the kind for the squad leader's, 0x3f + it for
	// the commander's; the kinds are the map's (4..7: frames 0..3 of the
	// six). A comm-rose order (0..3) is taken as its map kind.
	const int mapKind = kind < 4 ? kind + 4 : kind;
	return (fromCommander ? 0x3f : 0x39) + mapKind;
}

int requestIconId(int kind, bool selected)
{
	// 0x100f44d0: +0x60 the icon, +0x58 the selected one.
	switch (kind) {
	case 1: return selected ? 0x7d : 0x79;
	case 2: return selected ? 0x7f : 0x7b;
	case 3: return selected ? 0x80 : 0x7c;
	default: return selected ? 0x7e : 0x7a;
	}
}

int vehicleIconId(int type, int category, bool usePlayerIcon)
{
	switch (type) {
	case 0: case 1: return 0x5f;
	case 4: case 0x10: case 0x12: case 0x13: return 0x5d;
	case 5: return 0x5c;
	case 6: case 7: case 8: case 0x11: return 0x61;
	case 9: return 0x64;
	case 10: return 0x65;
	case 0xb: case 0xc: case 0xd: case 0xe: case 0x16: return 0x60;
	case 0xf: case 0x19: case 0x1a: return 0x5a;
	case 0x14: return 0x66;
	case 0x15: return 0x5e;
	case 0x17: return 0x62;
	case 0x18: return 0x63;
	default: break;
	}
	if (usePlayerIcon) {
		return 0x5a;
	}
	switch (category) {
	case 0: return 0x5b;
	case 1: return 0x60;
	case 2: return 0x61;
	case 3: return 0x62;
	default: return 0x5a;
	}
}

int vehicleTypeOf(const std::string& name)
{
	static const char* const kNames[] = {"VTHeavyTank", "VTLightTank", "VTArtillery", "VTOrbitalStrike", "VTApc",
		"VTScoutCar", "VTFighter", "VTDiveBomber", "VTBomber", "VTAAGun", "VTStationaryMG", "VTDestroyer",
		"VTBattleship", "VTCarrier", "VTSubmarine", "VTLcvp", "VTNoVehicleType", "VTLevelBomber", "VTArmoredCar",
		"VTTankHunter", "VTATGun", "VTAA", "VTBoat", "VTHelicopterAttack", "VTHelicopterTransport", "VTSniper",
		"VTInfantry"};
	for (int i = 0; i < 0x1b; ++i) {
		if (name == kNames[i]) {
			return i;
		}
	}
	return name.empty() ? 0x1b : std::atoi(name.c_str());
}

int playerIconId(bool squadLeader, int squad, bool commander)
{
	// 0x100f0310: slot 0x150 (leads a squad) ICObject 0x69 + the squad
	// (slot 0x120), slot 0x128 (commands) 0x69, else 0x5a.
	if (squadLeader) {
		return 0x69 + squad;
	}
	return commander ? 0x69 : 0x5a;
}

Abilities kitAbilities(const std::string& kitType)
{
	Abilities a;
	a.heal = kitType == "Assault";
	a.repair = kitType == "Engineer";
	a.ammo = kitType == "Support";
	return a;
}

void CallTimers::tick(float seconds)
{
	// 0x100f3490: each counts down to 0.
	medic = std::max(0.0f, medic - seconds);
	ammo = std::max(0.0f, ammo - seconds);
	repair = std::max(0.0f, repair - seconds);
}

int helperIcon(const CallTimers& t, const Abilities& helper)
{
	if (t.medic > 0 && helper.heal) {
		return 0x2d; // ICMedic
	}
	if (t.repair > 0 && helper.repair) {
		return 0x2e; // ICRepair
	}
	if (t.ammo > 0 && helper.ammo) {
		return 0x2f; // ICAmmo
	}
	return -1;
}

int callIcon(int code, const Abilities& local, float& seconds)
{
	seconds = 12; // DAT_101f6740
	switch (code) {
	case CallMedic: return local.heal ? 0x2d : -1;
	case CallAmmo: return local.ammo ? 0x2f : -1;
	case CallRepair: return local.repair ? 0x2e : -1;
	case CallRevive:
	case CallRevive + 1:
		seconds = 30; // 0x41f00000
		return local.heal ? 0x2c : -1; // ICRevive
	default: return -1;
	}
}

NameRow nameRow(int rank, bool veteran)
{
	NameRow row;
	row.rankIcon = rank >= 0 && rank < 44 ? rank : -1; // ICRank + rank (44 frames)
	row.veteranIcon = veteran ? 0x4f : -1;
	return row;
}

// --- The icon registry (nametags.con) ---

int IconRegistry::idOf(const std::string& name)
{
	// NametagItemIDs (0x10070e1a's names).
	static const std::pair<const char*, int> kIds[] = {{"ICRank", 0}, {"ICRevive", 0x2c}, {"ICMedic", 0x2d},
		{"ICRepair", 0x2e}, {"ICAmmo", 0x2f}, {"ICVehicleArmor", 0x30}, {"ICVehicleMedic", 0x31},
		{"ICVehicleRepair", 0x32}, {"ICVehicleAmmo", 0x33}, {"ICHealthBar", 0x34}, {"ICArmorBar", 0x35},
		{"ICAmmoBar", 0x36}, {"ICSquadOrder", 0x3d}, {"ICCommanderOrder", 0x43}, {"ICTarget", 0x4e}, {"ICVeteran", 0x4f},
		{"ICObject", 0x50}, {"ICObjectOrbitalStrikeRequestOrder", 0x79}, {"ICObjectOrbitalBombardmentRequestOrder", 0x7a},
		{"ICObjectUAVRequestOrder", 0x7b}, {"ICObjectSupplyRequestOrder", 0x7c},
		{"ICObjectOrbitalStrikeRequestOrderSelected", 0x7d}, {"ICObjectOrbitalBombardmentRequestOrderSelected", 0x7e},
		{"ICObjectUAVRequestOrderSelected", 0x7f}, {"ICObjectSupplyRequestOrderSelected", 0x80},
		{"ICObjectTeam1Titan", 0x81}, {"ICObjectTeam2Titan", 0x82}, {"ICObjectTeam1TitanBackground", 0x83},
		{"ICObjectTeam2TitanBackground", 0x84}, {"ICObjectTeam1TitanBoardable", 0x85}, {"ICObjectTeam2TitanBoardable", 0x86},
		{"ICObjectTeam1TitanBoardableBackground", 0x87}, {"ICObjectTeam2TitanBoardableBackground", 0x88},
		{"ICObjectCannon01", 0x8c}, {"ICObjectCannon02", 0x8d}, {"ICObjectCannon03", 0x8e}, {"ICObjectCannon04", 0x8f},
		{"ICObjectCannon05", 0x90}, {"ICObjectCannonBackground", 0x91}, {"ICObjectCoreBackground", 0x92},
		{"ICObjectFlagBackground", 0x93}, {"ICObjectConsoleBackground", 0x94}, {"ICObjectCore", 0x96},
		{"ICObjectConsole01", 0x97}, {"ICObjectConsole02", 0x98}, {"ICObjectConsole03", 0x99}, {"ICObjectConsole04", 0x9a},
		{"ICNetworkFriendlySmallCircle", 0xa0}, {"ICNetworkFriendlyBar", 0xa1}, {"ICNetworkFriendlyBarBg", 0xa2},
		{"ICNetworkEnemySmallCircle", 0xa3}, {"ICNetworkEnemyBar", 0xa4}, {"ICNetworkEnemyBarBg", 0xa5}};
	for (const auto& [n, id] : kIds) {
		if (name.size() == std::char_traits<char>::length(n)
			&& std::equal(name.begin(), name.end(), n, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) {
			return id;
		}
	}
	return -1;
}

void IconRegistry::setTexture(int rawFrames, const std::string& rawTexture, int x, int y, int w, int h)
{
	int frames = rawFrames;
	if (current < 0 || w <= 0 || h <= 0) {
		return;
	}
	std::string texture = rawTexture;
	std::replace(texture.begin(), texture.end(), '\\', '/');
	if (texture.find('.', texture.rfind('/') + 1) == std::string::npos) {
		texture += ".dds";
	}
	int tw = 0, th = 0;
	if (!textureSize || !textureSize(texture, tw, th) || tw <= 0 || th <= 0) {
		tw = x + w * std::max(frames, 1);
		th = y + h;
	}
	// 0x100ee9a0: a bar's (mode +0x4cc 1) first number picks its texture,
	// 0 the fill and 1 the base (kept as id and id | 0x100); an icon's is
	// its frame count (0x1016b790: frame k at x + k w).
	if (bar) {
		frames = 1;
	}
	const int base = bar && rawFrames == 1 ? current | 0x100 : current;
	for (int k = 0; k < std::max(frames, 1); ++k) {
		Icon icon;
		icon.texture = texture;
		icon.u0 = static_cast<float>(x + k * w) / static_cast<float>(tw);
		icon.u1 = static_cast<float>(x + (k + 1) * w) / static_cast<float>(tw);
		icon.v0 = static_cast<float>(y) / static_cast<float>(th);
		icon.v1 = static_cast<float>(y + h) / static_cast<float>(th);
		icon.width = static_cast<float>(w);
		icon.height = static_cast<float>(h);
		icons[base + k] = icon;
	}
}

void IconRegistry::bind(Console& console)
{
	const uint32_t access = ConsoleAccess::Anyone;
	bindMethod(console, "nametags.createIcon", access, [this](std::string name) {
		current = idOf(name);
		bar = false;
	});
	// createBar <id> <from> <to>: the fill's texels (ICArmorBar 1..45).
	bindMethod(console, "nametags.createBar", access, [this](std::string name, int from, int to) {
		current = idOf(name);
		bar = true;
		barSpan[current] = {from, to};
	});
	bindMethod(console, "nametags.setTexture", access, [this](int frames, std::string texture, int x, int y, int w, int h) {
		setTexture(frames, texture, x, y, w, h);
	});
}

Nametags::Nametags(const io::FileManager& files)
{
	std::vector<uint8_t> data;
	if (files.readFile("Fonts/BF1942.font", data)) {
		m_fontLoaded = m_font.parse(std::string(data.begin(), data.end()));
	}
}

// 0x100ef210: the view and projection; with glueToEdge a point behind the
// camera or outside the circle goes onto the circle.
bool Nametags::project(const View& view, const std::array<float, 3>& point, float& x, float& y, bool glue,
	float circleRadius, bool* clamped)
{
	const Vec f = forwardOf(view);
	const Vec r {std::cos(view.yaw), 0, -std::sin(view.yaw)};
	const Vec u {f.y * r.z - f.z * r.y, f.z * r.x - f.x * r.z, f.x * r.y - f.y * r.x};
	const Vec d = Vec {point[0], point[1], point[2]} - Vec {view.position[0], view.position[1], view.position[2]};
	const float depth = dot(d, f);
	const float t = std::tan(view.fovY / 2);
	float nx, ny;
	if (depth > 0.1f) {
		nx = dot(d, r) / (depth * t * view.aspect);
		ny = dot(d, u) / (depth * t);
	} else {
		if (!glue) {
			return false;
		}
		// Behind: towards its side, pushed onto the circle below.
		nx = dot(d, r);
		ny = dot(d, u);
		if (nx == 0 && ny == 0) {
			ny = -1;
		}
		const float scale = 1e3f / std::max(std::sqrt(nx * nx + ny * ny), 1e-6f);
		nx *= scale;
		ny *= scale;
	}
	if (clamped) {
		*clamped = false;
	}
	const float length = std::sqrt(nx * nx + ny * ny);
	if (glue && length > circleRadius) {
		nx *= circleRadius / length;
		ny *= circleRadius / length;
		if (clamped) {
			*clamped = true;
		}
	} else if (std::fabs(nx) > 1.05f || std::fabs(ny) > 1.05f) {
		return false;
	}
	x = 400 + nx * 400;
	y = 300 - ny * 300;
	return true;
}

const Nametags::TagTemplate& Nametags::tagTemplate(const world::ObjectTemplate* t)
{
	const auto known = m_templates.find(t);
	if (known != m_templates.end()) {
		return known->second;
	}
	TagTemplate tt;
	if (t) {
		const auto flag = [t](const char* name, bool otherwise) { return number(t, name, otherwise ? 1.0f : 0.0f) != 0; };
		tt.controlPoint = t->typeName() == "ControlPoint";
		tt.showItem = flag("Nametag.showItem", false);
		tt.show3dIcon = flag("Nametag.show3dIcon", false);
		tt.showDistance = flag("Nametag.showDistance", false);
		tt.glue = flag("Nametag.glueToEdge", false);
		tt.teamSpecific = flag("Nametag.teamSpecific", false);
		tt.inverse = flag("Nametag.inverseTeamSpecific", false);
		tt.tracker = flag("Nametag.showIconOnTracker", false);
		tt.showInside = flag("Nametag.showInside", false);
		tt.showOutside = flag("Nametag.showOutside", true);
		tt.selectable = flag("Nametag.selectable", false);
		tt.useTeamColor = flag("Nametag.useTeamColor", false);
		tt.useAliveArmor = flag("Nametag.useAliveArmor", false);
		tt.icon = static_cast<int>(number(t, "Nametag.3dMapIcon", -1));
		tt.selectedIcon = static_cast<int>(number(t, "Nametag.selected3dMapIcon", -1));
		tt.maxDistance = number(t, "Nametag.maxDistance", 1200);
		tt.maxEnemyDistance = number(t, "Nametag.maxEnemyDistance", 1200);
		if (const std::string* offset = setting(t, "Nametag.3dMapIconOffset")) {
			std::sscanf(offset->c_str(), "%f/%f/%f", &tt.offset[0], &tt.offset[1], &tt.offset[2]);
		}
		const std::string* type = setting(t, "setVehicleType");
		tt.vehicleIcon = vehicleIconId(vehicleTypeOf(type ? *type : std::string()), game::GameWorld::vehicleCategory(*t),
			flag("vehicleHud.usePlayerIcon", false));
	}
	return m_templates.emplace(t, tt).first->second;
}

void Nametags::radioCall(int caller, int code, bool local)
{
	m_pendingCalls.push_back({caller, code, local});
}

std::vector<hud::HudQuad> Nametags::frame(game::GameWorld& world, const world::ObjectManager& objects,
	const View& view, float seconds)
{
	std::vector<hud::HudQuad> out;
	if (!m_fontLoaded) {
		return out;
	}
	game::GameWorld::PlayerInfo me;
	if (!world.playerInfo(world.localPlayer(), me)) {
		return out;
	}
	// The manager's clock and the calls' timers (0x100f3490): the blink
	// 0.5 - 0.5 cos(2 pi t) (DAT_10211288, the rate DAT_10212250 =
	// 12 / 12 x 2 pi).
	m_clock = std::fmod(m_clock + seconds, 1.0f);
	const float blink = 0.5f - 0.5f * std::cos(m_clock * 6.2831855f);
	m_calls.tick(seconds);
	const Vec eye {view.position[0], view.position[1], view.position[2]};
	const Vec forward = forwardOf(view);
	const physics::Vec3 eyeP {eye.x, eye.y, eye.z};
	const auto kitTypeOf = [&](uint32_t kit) -> std::string {
		const world::Object* o = kit ? objects.find(kit) : nullptr;
		const std::string* type = setting(o ? o->objectTemplate() : nullptr, "kitType");
		return type ? *type : std::string();
	};
	const std::string myKitType = kitTypeOf(me.kit);
	const Abilities mine = kitAbilities(myKitType);
	const bool localMedic = myKitType == "Assault";
	const bool localEngineer = myKitType == "Engineer";
	const std::vector<game::GameWorld::PlayerInfo> players = world.playerList();

	// The calls heard (0x100f19c0): on the caller's tag within
	// maxCallDistance when the local kit can answer; the local player's
	// own start his timers.
	for (const PendingCall& call : m_pendingCalls) {
		if (call.local || call.caller == me.id) {
			float& timer = call.code == CallAmmo ? m_calls.ammo : call.code == CallRepair ? m_calls.repair : m_calls.medic;
			timer = 12;
			continue;
		}
		const auto it = m_players.find(call.caller);
		float time = 0;
		const int icon = callIcon(call.code, mine, time);
		if (icon < 0 || it == m_players.end() || it->second.distance < 0
			|| static_cast<float>(it->second.distance) >= m_settings.maxCallDistance) {
			continue;
		}
		Tag& tag = it->second;
		if (call.code >= CallRevive || tag.callTime <= 0) {
			tag.callTime = time;
		}
		tag.callIcon = icon;
	}
	m_pendingCalls.clear();

	// The tagged object's points (0x100efb90): the text's at a soldier's
	// head along its up (standing: 0.6 m over its origin, more with
	// distance), a vehicle's top (0.35 x its height + 0.45); the icon's
	// 0.6 m under the soldier's origin (the offset times 30 / d past
	// 30 m), a vehicle's origin.
	const auto headOf = [&](const game::GameWorld::PlayerInfo& p, float distance, std::array<float, 3>& at, std::array<float, 3>& icon) {
		if (p.vehicle) {
			const world::Object* v = objects.find(p.vehicle);
			if (!v) {
				return false;
			}
			// The render object's height (root +0x44 slot 0x44) isn't
			// available here: 4 m taken.
			constexpr float kVehicleHeight = 4;
			at = {v->position().x, v->position().y
					+ m_settings.vehicleNametagOffsetFactor * kVehicleHeight + m_settings.vehicleNametagOffset,
				v->position().z};
			icon = {v->position().x, v->position().y, v->position().z};
			return true;
		}
		// The soldier's origin taken 1 m over its feet (the original's
		// soldier is placed by its body). By the soldier's pose (its slot
		// 0x3c: 1 standing, 2 lying, 3 crouched, as the animation's
		// 0x008b9840 has 2 for prone) the text sits 0.6 / 0.3 / 0.7 m over
		// it, plus min(d x that x 0.05, 3); the icon 0.6 / 1.2 / 1.2 m under
		// it, times 30 / d past 30 m. Other poses (swimming): 1 m and none.
		float name = 0.6f, under = -0.6f;
		if (p.pose == game::SoldierPose::Prone) {
			name = 0.3f;
			under = -1.2f;
		} else if (p.pose == game::SoldierPose::Crouch) {
			name = 0.7f;
			under = -1.2f;
		} else if (p.pose == game::SoldierPose::Swim) {
			name = 1.0f;
			under = 0;
		}
		const float k = name + std::min(distance * name * 0.05f, 3.0f);
		at = {p.position.x, p.position.y + 1.0f + k, p.position.z};
		const float below = under * (distance > 30 ? 30 / distance : 1.0f);
		icon = {p.position.x, p.position.y + 1.0f + below, p.position.z};
		return true;
	};

	// The crosshair target (0x100ef5b0 every 0.5 s): what a ray 300 m along
	// the view meets; every occupant of a vehicle it meets is aimed at
	// (0x100f2ec0). Soldiers are taken as 0.6 m spheres about their
	// middle, vehicles as 2.5 m ones about their origin.
	m_crosshairCheck -= seconds;
	if (m_crosshairCheck <= 0) {
		m_crosshairCheck = 0.5f;
		m_crosshairTarget = 0;
		float best = m_settings.playerCullFar;
		for (const auto& p : players) {
			if (p.id == me.id || !p.alive) {
				continue;
			}
			const uint32_t object = p.vehicle ? p.vehicle : p.soldier;
			const world::Object* o = objects.find(object);
			if (!o || (me.vehicle && object == me.vehicle)) {
				continue;
			}
			const Vec centre {o->position().x, p.vehicle ? o->position().y : p.position.y + 1.0f, o->position().z};
			const Vec d = centre - eye;
			const float along = dot(d, forward);
			if (along <= 0 || along >= best) {
				continue;
			}
			const float off2 = dot(d, d) - along * along;
			const float radius = p.vehicle ? 2.5f : 0.6f;
			if (off2 > radius * radius) {
				continue;
			}
			if (world.lineOfSight(eyeP, {centre.x, centre.y, centre.z}, p.vehicle ? p.vehicle : me.vehicle)) {
				best = along;
				m_crosshairTarget = object;
			}
		}
	}

	struct Drawn {
		float distance;
		std::vector<hud::HudQuad> quads;
	};
	std::vector<Drawn> drawn;
	// The selection (0x100f16c0): the selectable tag nearest the screen's
	// centre within 0.08 (normalised device units); an object's shows its
	// selected3dMapIcon (0x50 +, ICObjectFlagBackground 0x93 without one,
	// 0x100f3ed0) under the icon in ColorArray's yellow (ColorIndex3 4).
	struct Candidate {
		float centre;
		size_t drawn;
		int background; // -1: none (players, +0x58)
		uint32_t object;
	};
	Candidate selection {0.08f, 0, -1, 0};
	const auto consider = [&](float x, float y, int background, uint32_t object) {
		const float nx = x / 400 - 1, ny = 1 - y / 300;
		const float c = std::sqrt(nx * nx + ny * ny);
		if (c > 0 && c < selection.centre) {
			selection = {c, drawn.size(), background, object};
		}
	};
	const auto iconQuad = [](const IconRegistry::Icon& icon, float x, float y, const float colour[4]) {
		hud::HudQuad q;
		q.texture = icon.texture;
		q.x0 = std::round(x - icon.width / 2);
		q.y0 = std::round(y - icon.height / 2);
		q.x1 = q.x0 + icon.width;
		q.y1 = q.y0 + icon.height;
		q.u0 = icon.u0;
		q.v0 = icon.v0;
		q.u1 = icon.u1;
		q.v1 = icon.v1;
		std::copy(colour, colour + 4, q.color);
		return q;
	};

	// The objects with tags, looked for again once a second (the
	// templates' Nametag fields are read once per template).
	m_taggedCheck -= seconds;
	if (m_taggedCheck <= 0) {
		m_taggedCheck = 1;
		m_tagged.clear();
		for (const auto& owned : objects.objects()) {
			if (owned && tagTemplate(owned->objectTemplate()).tagged()) {
				m_tagged.push_back(owned->id());
			}
		}
	}
	const std::vector<game::GameWorld::ControlPointInfo> points = world.controlPointList();
	for (const uint32_t id : m_tagged) {
		const world::Object* owned = objects.find(id);
		if (!owned || owned->hidden()) {
			continue;
		}
		const TagTemplate& tt = tagTemplate(owned->objectTemplate());
		if (tt.controlPoint) {
			// The control points' icons (object tags, 0x100f1330 / 0x100ef960).
			int team = 0;
			float nearest = 1e9f;
			for (const auto& point : points) {
				const float dx = point.position.x - owned->position().x, dz = point.position.z - owned->position().z;
				if (dx * dx + dz * dz < nearest) {
					nearest = dx * dx + dz * dz;
					team = point.team;
				}
			}
			if ((tt.teamSpecific && team != me.team) || (tt.inverse && team == me.team)) {
				continue;
			}
			const bool enemy = team != 0 && team != me.team;
			const float maxEnemy = std::min(tt.maxEnemyDistance, tt.inverse ? 1e9f : tt.maxDistance);
			const float most = enemy ? maxEnemy : tt.maxDistance;
			if (most == 0) {
				continue;
			}
			const std::array<float, 3> at {owned->position().x + tt.offset[0], owned->position().y + tt.offset[1],
				owned->position().z + tt.offset[2]};
			const float dx = at[0] - eye.x, dz = at[2] - eye.z;
			const float distance = std::sqrt(dx * dx + dz * dz);
			const float alpha = most < 0 ? 1.0f : clamp01(1 - distance / most);
			float x = 0, y = 0;
			bool clamped = false;
			if (alpha <= 0.01f || !project(view, at, x, y, tt.glue, m_settings.circleRadius, &clamped)) {
				continue;
			}
			// Off screen only glued with showIconOnTracker (0x100f3490's
			// skip of the tags out of the frustum).
			if (clamped && !tt.tracker && !project(view, at, x, y, false, 0)) {
				continue;
			}
			const int colour = tt.useTeamColor ? (team == 0 ? Neutral : enemy ? Enemy : Team) : Neutral;
			Drawn d {distance, {}};
			hud::HudQuad icon;
			icon.texture = std::string("Menu/Nametag/") + (team == 1 ? "Pac" : team == 2 ? "EU" : "Neutral") + "/miniMap_CP.dds";
			icon.x0 = std::round(x) - 16;
			icon.y0 = std::round(y) - 16;
			icon.x1 = icon.x0 + 32;
			icon.y1 = icon.y0 + 32;
			icon.u0 = icon.v0 = 0;
			icon.u1 = icon.v1 = 1;
			icon.color[0] = icon.color[1] = icon.color[2] = 1;
			icon.color[3] = alpha;
			d.quads.push_back(icon);
			if (tt.showDistance) {
				char text[32];
				std::snprintf(text, sizeof(text), "[%i m]", static_cast<int>(std::lround(distance)));
				float c[4];
				std::copy(kColours[colour], kColours[colour] + 4, c);
				c[3] = alpha;
				m_font.write(text, std::round(x - m_font.width(text) / 2), std::round(y + 16), c, d.quads);
			}
			if (tt.selectable) {
				consider(x, y, tt.selectedIcon < 0 ? 0x93 : 0x50 + tt.selectedIcon, id);
			}
			drawn.push_back(std::move(d));
			continue;
		}
		// The objects' 3D icons (Nametag.show3dIcon: the Titans' consoles and
		// core, the silos, the Titans themselves): the icon ICObject +
		// 3dMapIcon (0x50 + n, from nametags.con's frames) at the object and
		// its 3dMapIconOffset, faded out to maxDistance (maxEnemyDistance for
		// the enemy's); inside a Titan only those with showInside, outside
		// those with showOutside (default on); teamSpecific / inverse as the
		// control points'; with useAliveArmor none once the object's armour
		// is gone (the selectable pass, 0x1016a950: its armour's slot 0x60).
		const IconRegistry::Icon* icon = m_icons && tt.icon >= 0 ? m_icons->find(0x50 + tt.icon) : nullptr;
		if (!icon || (m_insideTitan ? !tt.showInside : !tt.showOutside)) {
			continue;
		}
		if (tt.useAliveArmor && (owned->destroyed() || world.hitPoints(id) == 0)) {
			continue;
		}
		const int team = owned->team();
		if ((tt.teamSpecific && team != me.team) || (tt.inverse && team == me.team)) {
			continue;
		}
		const bool enemy = team != 0 && team != me.team;
		const float most = enemy ? tt.maxEnemyDistance : tt.maxDistance;
		const std::array<float, 3> at {owned->position().x + tt.offset[0], owned->position().y + tt.offset[1],
			owned->position().z + tt.offset[2]};
		const float dx = at[0] - eye.x, dy = at[1] - eye.y, dz = at[2] - eye.z;
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		const float alpha = most <= 0 ? 0.0f : clamp01(1 - distance / most);
		float x = 0, y = 0;
		bool clamped = false;
		if (alpha <= 0.01f || !project(view, at, x, y, tt.glue, m_settings.circleRadius, &clamped)) {
			continue;
		}
		if (clamped && !tt.tracker && !project(view, at, x, y, false, 0)) {
			continue;
		}
		const int colour = tt.useTeamColor ? (team == 0 ? Neutral : enemy ? Enemy : Team) : Neutral;
		float c[4];
		std::copy(kColours[colour], kColours[colour] + 4, c);
		c[3] = alpha;
		if (tt.selectable) {
			consider(x, y, tt.selectedIcon < 0 ? 0x93 : 0x50 + tt.selectedIcon, id);
		}
		drawn.push_back({distance, {iconQuad(*icon, x, y, c)}});
	}

	// The squad's orders (TTOrder: the exe's order object 0x005b8b20 hands
	// the manager its point, slot 0x2c → 0x100f4620): ICSquadOrder 0x3d for
	// the squad leader's, ICCommanderOrder 0x43 for the commander's, its
	// frame the order's kind (0x39 / 0x3f + the map kind 4..7), at the
	// point 1.5 m up, white (+0x5c 0), with "[d m]" (flags 0x4079:
	// showDistance, glued to the edge with the tracker), faded out to 1200 m
	// (DAT_101f67b4) as the objects' icons. Only the local player's squad
	// has them.
	if (me.squad != 0 && m_icons) {
		const game::GameWorld::SquadOrder* orders[] = {world.squadLeaderOrder(me.team, me.squad), world.squadOrder(me.team, me.squad)};
		for (const game::GameWorld::SquadOrder* order : orders) {
			if (!order || order->type == game::GameWorld::OrderType::None) {
				continue;
			}
			const int id = orderIconId(order->fromCommander, static_cast<int>(order->type));
			const IconRegistry::Icon* icon = m_icons->find(id);
			if (!icon) {
				continue;
			}
			const std::array<float, 3> at {order->position.x, order->position.y + 1.5f, order->position.z};
			const float dx = at[0] - eye.x, dy = at[1] - eye.y, dz = at[2] - eye.z;
			const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
			const float alpha = clamp01(1 - distance / kOrderDistance);
			float x = 0, y = 0;
			if (alpha <= 0.01f || !project(view, at, x, y, true, m_settings.circleRadius)) {
				continue;
			}
			float c[4];
			std::copy(kColours[Neutral], kColours[Neutral] + 4, c);
			c[3] = alpha;
			Drawn d {distance, {iconQuad(*icon, x, y, c)}};
			char text[32];
			std::snprintf(text, sizeof(text), "[%i m]", static_cast<int>(std::lround(distance)));
			m_font.write(text, std::round(x - m_font.width(text) / 2), std::round(y + icon->height / 2), c, d.quads);
			drawn.push_back(std::move(d));
		}
	}
	// The commander's view of the squad leaders' requests (TTRequest:
	// 0x004c5720 → slot 0x24, 0x100f44d0, only for the local commander):
	// ICObjectOrbitalBombardmentRequestOrder 0x7a for kind 0, the orbital
	// strike's 0x79 for 1, the UAV's 0x7b for 2, the supply drop's 0x7c for
	// 3, selectable (0x100000: their Selected icons 0x7e / 0x7d / 0x7f /
	// 0x80). The rewrite's squad leaders send no requests (the request
	// rose's lines go only to the radio), so none is drawn.

	// The players' tags.
	std::map<int, Tag> seen;
	for (const auto& p : players) {
		if (p.id == me.id) {
			continue;
		}
		Tag tag = m_players.count(p.id) ? m_players[p.id] : Tag {};
		const world::Object* body = objects.find(p.vehicle ? p.vehicle : p.soldier);
		if (!body || p.cloaked) {
			continue;
		}
		const float dx = p.position.x - eye.x, dz = p.position.z - eye.z;
		float distance = std::sqrt(dx * dx + dz * dz);
		if (p.vehicle) {
			const float vx = body->position().x - eye.x, vz = body->position().z - eye.z;
			distance = std::sqrt(vx * vx + vz * vz);
		}
		tag.distance = shownDistance(tag.distance, distance);
		// A call's icon and its timer (+0x64, +0x6c: back to the tag's own
		// flags at 0).
		if (tag.callTime > 0) {
			tag.callTime -= seconds;
			if (tag.callTime <= 0) {
				tag.callTime = 0;
				tag.callIcon = -1;
			}
		}
		std::array<float, 3> head {}, iconAt {};
		if (!headOf(p, distance, head, iconAt)) {
			seen[p.id] = tag;
			continue;
		}
		// Aimed at (0x100f0d30): the crosshair's target, or (looked at
		// again every 0.25 s) its point within the 0.99999 cone and in
		// sight.
		tag.aimCheck -= seconds;
		const uint32_t object = p.vehicle ? p.vehicle : p.soldier;
		if (object == m_crosshairTarget) {
			tag.aimed = true;
		} else if (tag.aimCheck <= 0) {
			tag.aimCheck = 0.25f;
			const Vec d = Vec {head[0], head[1], head[2]} - eye;
			const float length = std::sqrt(dot(d, d));
			tag.aimed = length > 0 && dot(d, forward) / length > m_settings.aimCone
				&& world.lineOfSight(eyeP, {head[0], head[1], head[2]}, p.vehicle);
		}
		PlayerTagInput in;
		in.friendly = p.team == me.team;
		in.sameSquad = in.friendly && p.squad != 0 && p.squad == me.squad;
		in.commander = p.commander;
		in.alive = p.alive;
		in.manDown = p.manDown;
		in.sameVehicle = me.vehicle != 0 && p.vehicle == me.vehicle;
		in.aimedAt = tag.aimed;
		in.localMedic = localMedic;
		in.distance = distance;
		const bool calling = tag.callIcon >= 0 && in.friendly;
		const float alpha = playerTagAlpha(m_settings, in, tag.fade, seconds);
		seen[p.id] = tag;
		// The first occupant only speaks for a vehicle (0x100f33c0, bit
		// 0x10000).
		if (p.vehicle) {
			bool first = true;
			for (const auto& other : players) {
				if (other.alive && other.vehicle == p.vehicle && other.id != p.id && other.seat >= 0 && other.seat < p.seat) {
					first = false;
				}
			}
			if (!first) {
				continue;
			}
		}
		// Squad members' and callers' tags stay on screen (flags 0x4020 /
		// 0x26808: glueToEdge, showIconOnTracker).
		const bool glue = in.sameSquad || calling;
		float x = 0, y = 0, ix = 0, iy = 0;
		bool clamped = false;
		if (alpha <= 0.01f || !project(view, head, x, y, glue, m_settings.circleRadius, &clamped)) {
			continue;
		}
		Drawn d {distance, {}};
		float colour[4];
		const Colour index = playerTagColour(in);
		std::copy(kColours[index], kColours[index] + 4, colour);
		colour[3] = alpha;
		// A call blinks the text towards the highlight (ColorIndex2 = +0x68
		// 4, the crossfade (1 - min(d / 150, 1) / 2) x the blink).
		float textColour[4];
		std::copy(colour, colour + 4, textColour);
		const float fade = (1 - clamp01(distance / m_settings.maxCallDistance) * 0.5f) * blink;
		if (calling) {
			for (int k = 0; k < 3; ++k) {
				textColour[k] = colour[k] + (kColours[Highlight][k] - colour[k]) * fade;
			}
		}
		// The text (0x100f2b60): glued to the edge none (unless the call's
		// showDistance), "[d m]  name" with showDistance under 300 m, else
		// the name with "  [n/m]" for a vehicle's occupants.
		std::string text;
		if (!clamped || calling) {
			std::string suffix;
			if (p.vehicle) {
				int aboard = 0;
				for (const auto& other : players) {
					aboard += other.alive && other.vehicle == p.vehicle ? 1 : 0;
				}
				const size_t seats = world.vehicleSeats(p.vehicle).size();
				if (aboard > 1) {
					char buffer[32];
					std::snprintf(buffer, sizeof(buffer), "  [%d/%zu]", aboard, seats);
					suffix = buffer;
				}
			}
			if (clamped) {
				char buffer[32];
				std::snprintf(buffer, sizeof(buffer), "[%d m]", tag.distance);
				text = buffer;
			} else if (calling && static_cast<float>(tag.distance) < m_settings.playerCullFar) {
				char buffer[32];
				std::snprintf(buffer, sizeof(buffer), "[%d m]  ", tag.distance);
				text = buffer + p.name + suffix;
			} else {
				text = p.name + suffix;
			}
		}
		// The name's row (0x1016b100): the rank and veteran icons, 2
		// pixels, the text; centred, 16 pixels high; the icons white (the
		// squad's colour for a squad member).
		const NameRow row = clamped ? NameRow {} : nameRow(p.rank, false);
		std::vector<const IconRegistry::Icon*> rowIcons;
		float rowWidth = 0;
		for (const int id : {row.rankIcon, row.veteranIcon}) {
			const IconRegistry::Icon* i = id >= 0 && m_icons ? m_icons->find(id) : nullptr;
			if (i) {
				rowIcons.push_back(i);
				rowWidth += i->width;
			}
		}
		if (!text.empty() || !rowIcons.empty()) {
			const float iconsEnd = rowIcons.empty() ? 0 : rowWidth + 2;
			const float total = iconsEnd + m_font.width(text);
			float rx = std::round(x - total / 2);
			float iconColour[4] = {1, 1, 1, alpha};
			if (index == Squad) {
				std::copy(kColours[Squad], kColours[Squad] + 3, iconColour);
			}
			for (const IconRegistry::Icon* i : rowIcons) {
				d.quads.push_back(iconQuad(*i, rx + i->width / 2, std::round(y), iconColour));
				rx += i->width;
			}
			m_font.write(text, rx + (rowIcons.empty() ? 0 : 2), std::round(y - 8), textColour, d.quads);
		}
		// The player's icon (the selectable pass, 0x1016a950, flags
		// 0x500201 + 0x10 for the same team): +0x60, or crossfaded to the
		// call's icon +0x64 (the local player's own call shows who can
		// answer it), at the icon's point; glued to the edge with the
		// location arrow (Menu/Nametag/loactionArrow.dds, 0x10168d90:
		// outwards at 1.05 the edge, alpha 0.5).
		if (in.friendly && m_icons) {
			int own = playerIconId(p.squadLeader, p.squad, p.commander);
			if (own == 0x5a && p.vehicle) {
				// Aboard, the vehicle's own icon (0x100ee590 by its root's type).
				if (const world::Object* v = objects.find(p.vehicle)) {
					own = tagTemplate(v->objectTemplate()).vehicleIcon;
				}
			}
			int overlay = calling ? tag.callIcon : -1;
			if (overlay < 0) {
				overlay = helperIcon(m_calls, kitAbilities(kitTypeOf(p.kit)));
			}
			const IconRegistry::Icon* base = m_icons->find(own);
			const IconRegistry::Icon* flash = overlay >= 0 ? m_icons->find(overlay) : nullptr;
			const bool shown = clamped ? true : project(view, iconAt, ix, iy, false, 0);
			if (clamped) {
				ix = x;
				iy = y;
			}
			if (base && shown) {
				float c[4];
				std::copy(colour, colour + 4, c);
				const float cross = flash ? fade : 0;
				c[3] = alpha * (1 - cross);
				d.quads.push_back(iconQuad(*base, ix, iy, c));
				if (flash) {
					c[3] = alpha * cross;
					d.quads.push_back(iconQuad(*flash, ix, iy, c));
				}
				if (!clamped) {
					consider(ix, iy, -1, object);
				}
			}
			if (clamped) {
				const float ax = x - 400, ay = y - 300;
				hud::HudQuad arrow;
				arrow.texture = "Menu/Nametag/loactionArrow.dds";
				const float cx = 400 + ax * 1.05f, cy = 300 + ay * 1.05f;
				arrow.x0 = std::round(cx - 8);
				arrow.y0 = std::round(cy - 8);
				arrow.x1 = arrow.x0 + 16;
				arrow.y1 = arrow.y0 + 16;
				arrow.u0 = arrow.v0 = 0;
				arrow.u1 = arrow.v1 = 1;
				std::copy(colour, colour + 3, arrow.color);
				arrow.color[3] = 0.5f;
				arrow.rotation = std::atan2(-ay, -ax) * 180 / 3.14159265f; // the texture points left
				d.quads.push_back(arrow);
			}
		}
		// The bar (0x1016c1e0, ICArmorBar: bar_base under bar_filler, the
		// fill from texel 1 to 45 of 64): a hurt friend's health for a
		// medic, a damaged vehicle's armour for an engineer, under half.
		if (in.friendly && p.alive && !p.manDown && !clamped && (localMedic || (localEngineer && p.vehicle))) {
			const uint32_t armoured = localMedic && !p.vehicle ? p.soldier : p.vehicle;
			const world::Object* o = objects.find(armoured);
			const float hp = world.hitPoints(armoured);
			const world::ObjectTemplate* t = o ? o->objectTemplate() : nullptr;
			const float most = number(t, "armor.maxHitPoints", number(t, "armor.hitPoints", 100));
			const float fraction = most > 0 && hp >= 0 ? hp / most : 1.0f;
			if (fraction > 0 && fraction < 0.5f) {
				hud::HudQuad base;
				base.texture = "Menu/Nametag/bar_base.dds";
				base.x0 = std::round(x + 6 - 32);
				base.y0 = std::round(y - 8 - 9);
				base.x1 = base.x0 + 64;
				base.y1 = base.y0 + 8;
				base.u0 = base.v0 = 0;
				base.u1 = base.v1 = 1;
				base.color[0] = base.color[1] = base.color[2] = 1;
				base.color[3] = alpha;
				hud::HudQuad fill = base;
				fill.texture = "Menu/Nametag/bar_filler.dds";
				const float end = (1 + (45 - 1) * fraction) / 64;
				fill.x1 = fill.x0 + 64 * end;
				fill.u1 = end;
				d.quads.push_back(base);
				d.quads.push_back(fill);
			}
		}
		drawn.push_back(std::move(d));
	}
	m_players = std::move(seen);
	// The selected tag's background under its icon (its first quad).
	m_selectedObject = selection.centre < 0.08f ? selection.object : 0;
	if (m_selectedObject && selection.background >= 0 && m_icons && selection.drawn < drawn.size()
		&& !drawn[selection.drawn].quads.empty()) {
		if (const IconRegistry::Icon* back = m_icons->find(selection.background)) {
			const hud::HudQuad& front = drawn[selection.drawn].quads.front();
			float c[4];
			std::copy(kColours[Highlight], kColours[Highlight] + 4, c);
			c[3] = front.color[3];
			auto& quads = drawn[selection.drawn].quads;
			quads.insert(quads.begin(), iconQuad(*back, (front.x0 + front.x1) / 2, (front.y0 + front.y1) / 2, c));
		}
	}
	std::stable_sort(drawn.begin(), drawn.end(), [](const Drawn& a, const Drawn& b) { return a.distance > b.distance; });
	for (Drawn& d : drawn) {
		out.insert(out.end(), d.quads.begin(), d.quads.end());
	}
	return out;
}

} // namespace dice::hfe::nametags
