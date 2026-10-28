#include "Nametags.h"

#include "Game/GameWorld.h"
#include "IO/File/FileManager.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
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
	const float far = d * 1.0526f; // the fog factor (0x100f09b0) is left out
	return std::min(fade.alpha, clamp01(1 - (far - 200) / 100));
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
	const Vec eye {view.position[0], view.position[1], view.position[2]};
	const Vec forward = forwardOf(view);
	const physics::Vec3 eyeP {eye.x, eye.y, eye.z};
	const world::Object* myKit = me.kit ? objects.find(me.kit) : nullptr;
	const std::string* kitType = setting(myKit ? myKit->objectTemplate() : nullptr, "kitType");
	const bool localMedic = kitType && *kitType == "Assault";
	const bool localEngineer = kitType && *kitType == "Engineer";
	const std::vector<game::GameWorld::PlayerInfo> players = world.playerList();

	// The tagged object's point (0x100efb90): a soldier's head along its
	// up (standing: 0.6 m over its origin, more with distance), a
	// vehicle's top (0.35 x its height + 0.45).
	const auto headOf = [&](const game::GameWorld::PlayerInfo& p, float distance, std::array<float, 3>& at) {
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
			return true;
		}
		// The soldier's origin taken 1 m over its feet (the original's
		// soldier is placed by its body); pose 1 (standing) adds 0.6.
		const float k = 0.6f + std::min(distance * 0.6f * 0.05f, 3.0f);
		at = {p.position.x, p.position.y + 1.0f + k, p.position.z};
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

	// The control points' icons (object tags, 0x100f1330 / 0x100ef960).
	const std::vector<game::GameWorld::ControlPointInfo> points = world.controlPointList();
	for (const auto& owned : objects.objects()) {
		const world::ObjectTemplate* t = owned->objectTemplate();
		if (!t || t->typeName() != "ControlPoint" || number(t, "Nametag.showItem", 0) == 0
			|| owned->hidden()) {
			continue;
		}
		int team = 0;
		float nearest = 1e9f;
		for (const auto& point : points) {
			const float dx = point.position.x - owned->position().x, dz = point.position.z - owned->position().z;
			if (dx * dx + dz * dz < nearest) {
				nearest = dx * dx + dz * dz;
				team = point.team;
			}
		}
		const bool teamSpecific = number(t, "Nametag.teamSpecific", 0) != 0;
		const bool inverse = number(t, "Nametag.inverseTeamSpecific", 0) != 0;
		if ((teamSpecific && team != me.team) || (inverse && team == me.team)) {
			continue;
		}
		const bool enemy = team != 0 && team != me.team;
		const float maxDistance = number(t, "Nametag.maxDistance", 1200);
		const float maxEnemy = std::min(number(t, "Nametag.maxEnemyDistance", 1200), inverse ? 1e9f : maxDistance);
		const float most = enemy ? maxEnemy : maxDistance;
		if (most == 0) {
			continue;
		}
		std::array<float, 3> at {owned->position().x, owned->position().y, owned->position().z};
		if (const std::string* offset = setting(t, "Nametag.3dMapIconOffset")) {
			float ox = 0, oy = 0, oz = 0;
			std::sscanf(offset->c_str(), "%f/%f/%f", &ox, &oy, &oz);
			at = {at[0] + ox, at[1] + oy, at[2] + oz};
		}
		const float dx = at[0] - eye.x, dz = at[2] - eye.z;
		const float distance = std::sqrt(dx * dx + dz * dz);
		const float alpha = most < 0 ? 1.0f : clamp01(1 - distance / most);
		const bool glue = number(t, "Nametag.glueToEdge", 0) != 0;
		float x = 0, y = 0;
		if (alpha <= 0.01f || !project(view, at, x, y, glue, m_settings.circleRadius)) {
			continue;
		}
		const int colour = number(t, "Nametag.useTeamColor", 0) != 0 ? (team == 0 ? Neutral : enemy ? Enemy : Team)
																	: Neutral;
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
		if (number(t, "Nametag.showDistance", 0) != 0) {
			char text[32];
			std::snprintf(text, sizeof(text), "[%i m]", static_cast<int>(std::lround(distance)));
			float c[4];
			std::copy(kColours[colour], kColours[colour] + 4, c);
			c[3] = alpha;
			m_font.write(text, std::round(x - m_font.width(text) / 2), std::round(y + 16), c, d.quads);
		}
		drawn.push_back(std::move(d));
	}

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
		std::array<float, 3> head {};
		if (!headOf(p, distance, head)) {
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
		const float alpha = playerTagAlpha(m_settings, in, tag.fade, seconds);
		seen[p.id] = tag;
		float x = 0, y = 0;
		bool clamped = false;
		// Squad members' tags stay on screen (flags 0x4020: glueToEdge,
		// showIconOnTracker).
		if (alpha <= 0.01f || !project(view, head, x, y, in.sameSquad, m_settings.circleRadius, &clamped)) {
			continue;
		}
		Drawn d {distance, {}};
		float colour[4];
		const Colour index = playerTagColour(in);
		std::copy(kColours[index], kColours[index] + 4, colour);
		colour[3] = alpha;
		std::string text = clamped ? std::string() : p.name;
		// "  [%d/%d]": the occupants of a vehicle with more than one
		// (0x100f2b60).
		if (p.vehicle && !clamped) {
			int aboard = 0;
			for (const auto& other : players) {
				aboard += other.alive && other.vehicle == p.vehicle ? 1 : 0;
			}
			const size_t seats = world.vehicleSeats(p.vehicle).size();
			if (aboard > 1) {
				char suffix[32];
				std::snprintf(suffix, sizeof(suffix), "  [%d/%zu]", aboard, seats);
				text += suffix;
			}
		}
		if (clamped) {
			char edge[32];
			std::snprintf(edge, sizeof(edge), "[%d m]", tag.distance);
			text = edge;
		}
		const float width = m_font.width(text);
		m_font.write(text, std::round(x - width / 2), std::round(y - 8), colour, d.quads);
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
	std::stable_sort(drawn.begin(), drawn.end(), [](const Drawn& a, const Drawn& b) { return a.distance > b.distance; });
	for (Drawn& d : drawn) {
		out.insert(out.end(), d.quads.begin(), d.quads.end());
	}
	return out;
}

} // namespace dice::hfe::nametags
