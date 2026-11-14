// enhanced.botTitan: the bots' play aboard a Titan. Not retail: AIDLL has no
// strategy for Titan interiors and the shipped levels carry no navigation
// mesh for them. A bot aboard a Titan walks a grid sampled from the Titan's
// own soldier collision mesh (so it holds for either Titan in any mod):
//   - aboard the enemy's: to a spot that sees the nearest whole console and
//     shoots it; with the consoles gone, the same for the core;
//   - aboard its own while enemies are aboard: to the whole console nearest
//     to them;
//   - an enemy in sight is fought first (the ordinary fire behaviour).
// The grid is in the Titan's frame and is rebuilt when a part goes down
// (the core's doors open the way to the core).
#include "Bots.h"

#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <queue>

namespace dice::hfe::game {

namespace {

constexpr float kCell = 0.75f;       // shortcut: a fixed grid; a soldier is 0.6 m wide, upgrade to a mesh if bots snag
constexpr float kStep = 0.8f;        // the rise one cell may take (stairs, ramps)
constexpr float kDrop = 5.0f;        // a ledge a bot may jump down
constexpr float kHeadroom = 1.85f;
constexpr float kTop = 70.0f, kBottom = -70.0f; // the Titan's frame, about its origin
constexpr float kFireFar = 14.0f, kFireNear = 4.0f;
constexpr float kInfinity = std::numeric_limits<float>::infinity();

// Inside the box the grid of a Titan covers.
bool aboard(const GameWorld::TitanInfo& titan, const physics::Vec3& at)
{
	const physics::Vec3 d = at - titan.position;
	return d.x * d.x + d.z * d.z < 160.0f * 160.0f && d.y > kBottom && d.y < kTop;
}

float horizontal(const physics::Vec3& d)
{
	return std::sqrt(d.x * d.x + d.z * d.z);
}

// A seat with a pod launcher among its weapons (the APCs' pod seats, every seat of an air transport).
bool podSeat(const GameWorld::SeatLayout& seat)
{
	return seat.podLauncher || std::any_of(seat.weapons.begin(), seat.weapons.end(), [](const auto& w) { return w.podLauncher; });
}

const GameWorld::TitanInfo* titanOf(const std::vector<GameWorld::TitanInfo>& titans, int team)
{
	for (const GameWorld::TitanInfo& t : titans) {
		if (t.team == team && !t.wreck) {
			return &t;
		}
	}
	return nullptr;
}

} // namespace

struct BotManager::TitanGrid {
	uint32_t version = 0;
	float minX = 0, minZ = 0;
	int nx = 0, nz = 0;
	struct Cell {
		physics::Vec3 at; // in the Titan's frame, on the floor
		std::vector<std::pair<int, float>> out, in;
	};
	std::vector<Cell> cells;
	std::vector<std::vector<int>> columns;
	std::map<uint32_t, std::vector<float>> fields; // a part's id: each cell's cost to a spot that sees it
	std::vector<int> landing;                      // titanLandingSpots' cells
	bool landingDone = false;
	std::map<uint32_t, std::pair<physics::Vec3, float>> hunted; // a boarder's field: where he was (Titan frame) and when it was made

	const std::vector<int>* column(int ix, int iz) const
	{
		return ix < 0 || iz < 0 || ix >= nx || iz >= nz ? nullptr : &columns[static_cast<size_t>(iz) * nx + ix];
	}
	// The cell under a point (its feet): the nearest floor within 1.5 m in the 3x3 columns about it.
	int cellAt(const physics::Vec3& p) const
	{
		const int cx = static_cast<int>(std::floor((p.x - minX) / kCell + 0.5f)), cz = static_cast<int>(std::floor((p.z - minZ) / kCell + 0.5f));
		int best = -1;
		float bestD = 1e9f;
		for (int dz = -1; dz <= 1; ++dz) {
			for (int dx = -1; dx <= 1; ++dx) {
				if (const std::vector<int>* col = column(cx + dx, cz + dz)) {
					for (int c : *col) {
						const physics::Vec3 d = cells[static_cast<size_t>(c)].at - p;
						if (std::fabs(d.y) < 1.5f && physics::lengthSquared(d) < bestD) {
							bestD = physics::lengthSquared(d);
							best = c;
						}
					}
				}
			}
		}
		return best;
	}
};

BotManager::TitanGrid& BotManager::titanGrid(const GameWorld::TitanInfo& titan, int team)
{
	std::shared_ptr<TitanGrid>& held = m_titanGrids[{titan.object, team}];
	const uint32_t version = m_world.titanMeshVersion(titan.object);
	if (held && held->version == version) {
		return *held;
	}
	held = std::make_shared<TitanGrid>();
	TitanGrid& g = *held;
	g.version = version;
	const auto ray = [&](const physics::Vec3& a, const physics::Vec3& b, float& f) { return m_world.titanLocalRay(titan.object, team, a, b, f); };
	// shortcut: a fixed 160 m half-extent (the info's radius is shorter than the hull); upgrade to the mesh's box.
	const float r = std::max(titan.radius, 160.0f);
	g.minX = g.minZ = -r;
	g.nx = g.nz = static_cast<int>(2 * r / kCell) + 1;
	g.columns.resize(static_cast<size_t>(g.nx) * g.nz);
	// The floors: down each column, every surface with a soldier's headroom over it.
	for (int iz = 0; iz < g.nz; ++iz) {
		for (int ix = 0; ix < g.nx; ++ix) {
			const float x = g.minX + ix * kCell, z = g.minZ + iz * kCell;
			float y = kTop, f = 1;
			for (int n = 0; n < 24 && y > kBottom + 1 && ray({x, y, z}, {x, kBottom, z}, f); ++n) {
				const float floor = y + (kBottom - y) * f;
				float up = 1;
				if (!ray({x, floor + 0.35f, z}, {x, floor + kHeadroom, z}, up)) {
					g.columns[static_cast<size_t>(iz) * g.nx + ix].push_back(static_cast<int>(g.cells.size()));
					g.cells.push_back({{x, floor, z}, {}, {}});
				}
				y = floor - 0.25f;
			}
		}
	}
	// The steps between neighbours: level or a stair's rise with the body's
	// line clear at the knee and the head; a ledge down, one way.
	for (int iz = 0; iz < g.nz; ++iz) {
		for (int ix = 0; ix < g.nx; ++ix) {
			for (int a : g.columns[static_cast<size_t>(iz) * g.nx + ix]) {
				const physics::Vec3 pa = g.cells[static_cast<size_t>(a)].at;
				for (int dz = -1; dz <= 1; ++dz) {
					for (int dx = -1; dx <= 1; ++dx) {
						const std::vector<int>* col = dx || dz ? g.column(ix + dx, iz + dz) : nullptr;
						if (!col) {
							continue;
						}
						for (int b : *col) {
							const physics::Vec3 pb = g.cells[static_cast<size_t>(b)].at;
							const float dy = pb.y - pa.y, length = std::sqrt(physics::lengthSquared(pb - pa));
							float f = 1;
							if (std::fabs(dy) <= kStep) {
								if (!ray(pa + physics::Vec3 {0, 0.6f, 0}, pb + physics::Vec3 {0, 0.6f, 0}, f)
									&& !ray(pa + physics::Vec3 {0, 1.6f, 0}, pb + physics::Vec3 {0, 1.6f, 0}, f)) {
									g.cells[static_cast<size_t>(a)].out.push_back({b, length});
								}
							} else if (dy < 0 && dy >= -kDrop && (dx == 0 || dz == 0)) {
								const physics::Vec3 over {pb.x, pa.y + 1.0f, pb.z};
								if (!ray(pa + physics::Vec3 {0, 1.0f, 0}, over, f) && !ray(over, pb + physics::Vec3 {0, 0.5f, 0}, f)) {
									g.cells[static_cast<size_t>(a)].out.push_back({b, length + 2});
								}
							}
						}
					}
				}
			}
		}
	}
	// Beside a wall or an edge a step costs double: the ways keep to the middle.
	for (size_t a = 0; a < g.cells.size(); ++a) {
		for (auto& [b, cost] : g.cells[a].out) {
			if (g.cells[static_cast<size_t>(b)].out.size() < 7) {
				cost *= 2;
			}
		}
	}
	for (size_t a = 0; a < g.cells.size(); ++a) {
		for (const auto& [b, cost] : g.cells[a].out) {
			g.cells[static_cast<size_t>(b)].in.push_back({static_cast<int>(a), cost});
		}
	}
	if (const char* dump = std::getenv("BF2142_TITAN_GRID_DUMP")) { // DIAG
		if (FILE* f = std::fopen((std::string(dump) + "/grid-" + std::to_string(titan.object) + "-" + std::to_string(team) + ".txt").c_str(), "w")) {
			for (size_t a = 0; a < g.cells.size(); ++a) {
				std::fprintf(f, "%.2f %.2f %.2f", g.cells[a].at.x, g.cells[a].at.y, g.cells[a].at.z);
				for (const auto& [b, cost] : g.cells[a].out) std::fprintf(f, " %d", b);
				std::fprintf(f, "\n");
			}
			std::fclose(f);
		}
	}
	return g;
}

// Each cell's cost to a spot that sees the point (in the Titan's frame)
// from kFireNear..kFireFar; without such a spot, to the cells nearest it.
// Entry fields instead end inside the authored interaction sphere. With
// `around`, the cells under that object's collision box (an aircraft parked
// on the deck, which the grid was built without) are walked out of but not
// through.
const std::vector<float>& BotManager::titanField(TitanGrid& g, const GameWorld::TitanInfo& titan, int team, uint32_t part,
	const physics::Vec3& point, bool nearestOnly, float entryRadius, uint32_t around)
{
	const auto found = g.fields.find(part);
	if (found != g.fields.end()) {
		return found->second;
	}
	std::vector<float>& field = g.fields[part];
	field.assign(g.cells.size(), kInfinity);
	std::vector<char> blocked(g.cells.size(), 0);
	if (physics::Vec3 low, high; around && m_world.objectBox(around, low, high)) {
		for (size_t c = 0; c < g.cells.size(); ++c) {
			const physics::Vec3 at = m_world.titanToLocal(around, m_world.titanToWorld(titan.object, g.cells[c].at + physics::Vec3 {0, 1, 0}));
			blocked[c] = at.x > low.x - 0.5f && at.x < high.x + 0.5f && at.z > low.z - 0.5f && at.z < high.z + 0.5f
				&& at.y > low.y - 2 && at.y < high.y + 1;
		}
	}
	using Entry = std::pair<float, int>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
	float nearest = 1e9f;
	int nearestCell = -1;
	for (size_t c = 0; c < g.cells.size(); ++c) {
		const physics::Vec3 eye = g.cells[c].at + physics::Vec3 {0, 1.5f, 0};
		const float d = std::sqrt(physics::lengthSquared(point - eye));
		if (blocked[c]) {
			continue;
		}
		if (d < nearest && !g.cells[c].out.empty()) {
			nearest = d;
			nearestCell = static_cast<int>(c);
		}
		float f = 1;
		// Leave room for cellAt's snap: reaching a terminal cell must also
		// put the actual soldier inside the interaction sphere.
		const float reach = std::max(0.0f, entryRadius - kCell);
		const bool inEntry = entryRadius > 0
			&& physics::lengthSquared(point - (g.cells[c].at + physics::Vec3 {0, 1, 0})) <= reach * reach;
		if (inEntry || (!nearestOnly && d >= kFireNear && d <= kFireFar && (!m_world.titanLocalRay(titan.object, team, eye, point, f) || f * d > d - 1.0f))) {
			field[c] = 0;
			open.push({0, static_cast<int>(c)});
		}
	}
	if (open.empty() && nearestCell >= 0 && entryRadius <= 0) {
		field[static_cast<size_t>(nearestCell)] = 0;
		open.push({0, nearestCell});
	}
	while (!open.empty()) {
		const auto [cost, c] = open.top();
		open.pop();
		if (cost > field[static_cast<size_t>(c)] || (blocked[static_cast<size_t>(c)] && cost > 0)) {
			continue;
		}
		for (const auto& [from, step] : g.cells[static_cast<size_t>(c)].in) {
			if (cost + step < field[static_cast<size_t>(from)]) {
				field[static_cast<size_t>(from)] = cost + step;
				open.push({cost + step, from});
			}
		}
	}
	if (std::getenv("BF2142_BOT_STATS") && !(part & 0x80000000u)) { // DIAG
		size_t zero = 0, reach = 0;
		for (float f : field) {
			zero += f == 0;
			reach += f < kInfinity;
		}
		std::fprintf(stderr, "titan field: titan %u team %d part %u at %.1f %.1f %.1f: %zu firing cells, %zu of %zu reach, nearest cell %.1f m\n", titan.object, team, part,
			point.x, point.y, point.z, zero, reach, field.size(), nearest);
	}
	return field;
}

bool BotManager::titanGunWalk(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds,
	const physics::Vec3& entry, float radius, int seat)
{
	if (!m_settings.titanPlay()) {
		return false;
	}
	const auto titans = m_world.titans();
	const auto* titan = titanOf(titans, self.team);
	if (!titan) {
		return false;
	}
	TitanGrid& grid = titanGrid(*titan, self.team);
	// Gun-seat fields have their own keys, separate from object/boarder fields.
	const auto& field = titanField(grid, *titan, self.team, 0x40000000u + static_cast<uint32_t>(seat),
		m_world.titanToLocal(titan->object, entry), true, radius);
	bot.behaviour = "TitanGun";
	return titanWalk(bot, self, input, seconds, grid, *titan, field);
}

bool BotManager::titanPlay(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	if (!m_settings.titanPlay() || self.vehicle || m_world.gameMode() != "gpm_ti") {
		return false;
	}
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	const GameWorld::TitanInfo* titan = nullptr;
	for (const GameWorld::TitanInfo& t : titans) {
		const physics::Vec3 d = self.position - t.position;
		if (!t.wreck && d.x * d.x + d.z * d.z < 160.0f * 160.0f && d.y > kBottom && d.y < kTop) {
			titan = &t;
		}
	}
	if (!titan) {
		return false;
	}
	m_behaviourSeconds[titan->team == self.team ? "~T aboard own" : "~T aboard enemy"] += seconds; // DIAG
	const bool own = titan->team == self.team;
	// The enemies aboard this Titan.
	std::vector<const GameWorld::PlayerInfo*> boarders;
	if (own) {
		for (const GameWorld::PlayerInfo& p : m_players) {
			const physics::Vec3 d = p.position - titan->position;
			if (p.alive && p.team != self.team && !p.vehicle && d.x * d.x + d.z * d.z < 160.0f * 160.0f && d.y > kBottom && d.y < kTop) {
				boarders.push_back(&p);
			}
		}
		if (boarders.empty()) {
			// No one to fight: a boarder walks to the transport on the deck and
			// gets in; a defender stands by a console; the rest take the guns
			// and the pods, as before.
			const int role = titanRole(self);
			const GameWorld::TitanInfo* enemy = titanOf(titans, 3 - self.team);
			// (Not while a spawn vehicle of the team is at the enemy's Titan: the next life starts there.)
			const auto forward = [&] {
				for (uint32_t vehicle : m_world.spawnVehicles(self.team)) {
					for (const GameWorld::VehicleInfo& v : m_vehicles) {
						if (v.object == vehicle && v.working && v.freeSeats > 0 && horizontal(v.position - enemy->position) < 200) {
							return true;
						}
					}
				}
				return false;
			};
			const GameWorld::VehicleInfo* transport = role == 1 && enemy && !enemy->shield && !forward() ? titanTransport(self.team, *titan) : nullptr;
			if (!transport && role != 2) {
				return false;
			}
			TitanGrid& g = titanGrid(*titan, self.team);
			if (transport) {
				bot.behaviour = "TitanEmbark";
				const physics::Vec3 d = transport->position - self.position;
				input.use = horizontal(d) < 7 && std::fabs(d.y) < 5 && std::fmod(m_time, 0.6f) < 0.1f;
				titanWalk(bot, self, input, seconds, g, *titan,
					titanField(g, *titan, self.team, transport->object, m_world.titanToLocal(titan->object, transport->position), true));
				return true;
			}
			std::vector<const GameWorld::TitanPartInfo*> posts;
			for (const GameWorld::TitanPartInfo& part : m_world.titanParts(titan->team)) {
				if (part.kind == GameWorld::TitanPartInfo::Console && !part.wreck) {
					posts.push_back(&part);
				}
			}
			if (posts.empty()) {
				return false;
			}
			const GameWorld::TitanPartInfo& post = *posts[static_cast<size_t>(self.id) % posts.size()];
			bot.behaviour = "TitanDefend";
			titanWalk(bot, self, input, seconds, g, *titan,
				titanField(g, *titan, self.team, post.id, m_world.titanToLocal(titan->object, post.position)));
			return true;
		}
	}
	// An enemy in sight: the ordinary fight, standing (no mesh to walk here).
	if (fireUrgency(bot, self, m_players) > 0 && bot.target >= 0) {
		const auto known = std::find_if(bot.known.begin(), bot.known.end(), [&](const Known& k) { return k.player == bot.target; });
		if (known != bot.known.end() && known->lostAt < 0) {
			bot.behaviour = "Fire";
			fire(bot, self, input, seconds);
			input.throttle = input.strafe = 0;
			handleWeapon(bot, self, input);
			return true;
		}
	}
	TitanGrid& g = titanGrid(*titan, self.team);
	const physics::Vec3 local = m_world.titanToLocal(titan->object, self.position);
	const int cell = g.cellAt(local);
	if (cell < 0) {
		m_behaviourSeconds[own ? "~T own nocell" : "~T enemy nocell"] += seconds; // DIAG
		return false;
	}
	// Two defenders in three go after the nearest boarder: to a spot that
	// sees him from kFireNear..kFireFar (the fight above takes over once he
	// is in sight). His field is made anew a second on, once he has moved 3 m.
	if (own && self.id % 3 != 0) {
		const GameWorld::PlayerInfo* prey = boarders.front();
		for (const GameWorld::PlayerInfo* p : boarders) {
			prey = physics::lengthSquared(p->position - self.position) < physics::lengthSquared(prey->position - self.position) ? p : prey;
		}
		const uint32_t key = 0x80000000u | static_cast<uint32_t>(prey->id);
		const physics::Vec3 at = m_world.titanToLocal(titan->object, prey->position + physics::Vec3 {0, 1.2f, 0});
		auto& [seed, made] = g.hunted[key];
		if (!g.fields.count(key) || (m_time - made > 1 && physics::lengthSquared(at - seed) > 9)) {
			g.fields.erase(key);
			seed = at;
			made = m_time;
		}
		const std::vector<float>& f = titanField(g, *titan, self.team, key, seed);
		if (f[static_cast<size_t>(cell)] < kInfinity) {
			bot.behaviour = "TitanHunt";
			if (f[static_cast<size_t>(cell)] <= 0) {
				aim(bot, self, prey->position + physics::Vec3 {0, 1.2f, 0}, {}, true, {}, input, seconds); // facing him
			}
			return titanWalk(bot, self, input, seconds, g, *titan, f);
		}
	}
	// The objective: the cheapest whole console to reach; none left, the core.
	const std::vector<GameWorld::TitanPartInfo> parts = m_world.titanParts(titan->team);
	const GameWorld::TitanPartInfo* goal = nullptr;
	const std::vector<float>* field = nullptr;
	float best = kInfinity;
	const auto consider = [&](const GameWorld::TitanPartInfo& part) {
		const std::vector<float>& f = titanField(g, *titan, self.team, part.id, m_world.titanToLocal(titan->object, part.position));
		float cost = f[static_cast<size_t>(cell)];
		if (own && cost < kInfinity) {
			// A defender's console: the one the boarders are nearest to.
			cost = kInfinity;
			for (const GameWorld::PlayerInfo* p : boarders) {
				cost = std::min(cost, physics::lengthSquared(p->position - part.position));
			}
		}
		if (cost < best) {
			best = cost;
			goal = &part;
			field = &f;
		}
	};
	for (const GameWorld::TitanPartInfo& part : parts) {
		if (part.kind == GameWorld::TitanPartInfo::Console && !part.wreck) {
			consider(part);
		}
	}
	if (!goal) {
		for (const GameWorld::TitanPartInfo& part : parts) {
			if (part.kind == GameWorld::TitanPartInfo::Core && !part.wreck) {
				consider(part);
			}
		}
	}
	if (!goal) {
		static int said = 0;
		if (said++ < 6 && std::getenv("BF2142_BOT_STATS")) { // DIAG
			std::fprintf(stderr, "titan nogoal: bot local %.1f %.1f %.1f cell %d out %zu; parts:", local.x, local.y, local.z, cell, g.cells[static_cast<size_t>(cell)].out.size());
			for (const auto& part : parts) std::fprintf(stderr, " %s/%d/%d", part.name.c_str(), static_cast<int>(part.kind), part.wreck ? 1 : 0);
			std::fprintf(stderr, "\n");
		}
		m_behaviourSeconds[own ? "~T own nogoal" : "~T enemy nogoal"] += seconds; // DIAG
		return false;
	}
	bot.behaviour = own ? "TitanDefend" : "TitanAttack";
	const float here = (*field)[static_cast<size_t>(cell)];
	if (here <= 0) {
		// At a spot that sees it: a defender waits; an attacker shoots in
		// bursts (a tap for a single-shot weapon, a burst for an automatic).
		if (!own) {
			aim(bot, self, goal->position, {}, true, {}, input, seconds);
			const physics::Vec3 d = goal->position - (self.position + physics::Vec3 {0, 1.6f, 0});
			const float yawOff = std::remainder(std::atan2(d.x, d.z) * 57.29578f - (self.yaw + input.turn), 360.0f);
			input.fire = std::fabs(yawOff) < 4 && std::fmod(m_time, 0.8f) < 0.6f && self.roundsInMag != 0 && !self.reloading;
			input.reload = self.roundsInMag == 0 && !bot.reloadedLast;
		}
		input.throttle = input.strafe = 0;
		handleWeapon(bot, self, input);
		return true;
	}
	return titanWalk(bot, self, input, seconds, g, *titan, *field);
}

// A step down a field of the grid; false where it has no way on (or the bot is off the grid).
bool BotManager::titanWalk(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds, TitanGrid& g,
	const GameWorld::TitanInfo& titanInfo, const std::vector<float>& fieldRef)
{
	const GameWorld::TitanInfo* titan = &titanInfo;
	const std::vector<float>* field = &fieldRef;
	const physics::Vec3 local = m_world.titanToLocal(titan->object, self.position);
	const int cell = g.cellAt(local);
	if (cell < 0) {
		return false;
	}
	if ((*field)[static_cast<size_t>(cell)] <= 0) {
		handleWeapon(bot, self, input);
		return true; // there
	}
	// Down the field: the cheapest neighbour, and the one after it once close.
	const auto next = [&](int from) {
		int to = -1;
		float cost = (*field)[static_cast<size_t>(from)];
		for (const auto& [b, step] : g.cells[static_cast<size_t>(from)].out) {
			if ((*field)[static_cast<size_t>(b)] + step < cost + step && (*field)[static_cast<size_t>(b)] < cost) {
				cost = (*field)[static_cast<size_t>(b)];
				to = b;
			}
		}
		return to;
	};
	int to = next(cell);
	if (to < 0) {
		return false;
	}
	if (const int after = next(to); after >= 0 && physics::lengthSquared(g.cells[static_cast<size_t>(to)].at - local) < 0.5f * 0.5f) {
		to = after;
	}
	const physics::Vec3 d = m_world.titanToWorld(titan->object, g.cells[static_cast<size_t>(to)].at) - self.position;
	const float off = std::remainder(std::atan2(d.x, d.z) * 57.29578f - self.yaw, 360.0f);
	input.turn = std::clamp(off, -360 * seconds, 360 * seconds);
	input.throttle = std::fabs(off) < 60 ? 1.0f : 0.0f;
	// No way made in 2 s: a jump, and a step aside.
	if (physics::lengthSquared(self.position - bot.titanAt) > 1.0f) {
		bot.titanAt = self.position;
		bot.titanAtTime = m_time;
	} else if (m_time - bot.titanAtTime > 2) {
		input.jump = std::fmod(m_time, 1.0f) < 0.2f;
		input.strafe = std::fmod(m_time, 4.0f) < 2 ? 1.0f : -1.0f;
	}
	handleWeapon(bot, self, input);
	return true;
}

// Where a boarder can come down: cells under the open sky, clear all round,
// from which a whole console (or the core) can be walked to. World frame.
std::vector<physics::Vec3> BotManager::titanLandingSpots(int titanTeam, int team)
{
	std::vector<physics::Vec3> spots;
	for (const GameWorld::TitanInfo& titan : m_world.titans()) {
		if (titan.team != titanTeam || titan.wreck) {
			continue;
		}
		TitanGrid& g = titanGrid(titan, team);
		if (!g.landingDone) {
			g.landingDone = true;
			std::vector<float> best(g.cells.size(), kInfinity);
			for (const GameWorld::TitanPartInfo& part : m_world.titanParts(titan.team)) {
				if ((part.kind == GameWorld::TitanPartInfo::Console || part.kind == GameWorld::TitanPartInfo::Core) && !part.wreck) {
					const std::vector<float>& f = titanField(g, titan, team, part.id, m_world.titanToLocal(titan.object, part.position));
					for (size_t c = 0; c < f.size(); ++c) {
						best[c] = std::min(best[c], f[c]);
					}
				}
			}
			for (size_t c = 0; c < g.cells.size(); ++c) {
				float f = 1;
				const physics::Vec3 at = g.cells[c].at;
				if (best[c] < kInfinity && g.cells[c].out.size() >= 8
					&& !m_world.titanLocalRay(titan.object, team, at + physics::Vec3 {0, 0.4f, 0}, {at.x, kTop, at.z}, f)) {
					g.landing.push_back(static_cast<int>(c));
				}
			}
			// Four cells (3 m) in from any edge or wall: a pod comes down fast and wide.
			for (int pass = 0; pass < 4; ++pass) {
				std::vector<char> in(g.cells.size(), 0);
				for (int c : g.landing) {
					in[static_cast<size_t>(c)] = 1;
				}
				std::vector<int> kept;
				for (int c : g.landing) {
					const auto& out = g.cells[static_cast<size_t>(c)].out;
					if (std::all_of(out.begin(), out.end(), [&](const auto& link) { return in[static_cast<size_t>(link.first)] != 0; })) {
						kept.push_back(c);
					}
				}
				g.landing.swap(kept);
			}
		}
		for (int c : g.landing) {
			spots.push_back(m_world.titanToWorld(titan.object, g.cells[static_cast<size_t>(c)].at));
		}
	}
	return spots;
}

// Boarding by drop pod (the APCs' pod seats). With the enemy Titan's shield
// down: a pod is launched from 25..130 m beside a landing spot under an
// open sky (titanPodReady); its rider steers it over the spot; an APC's
// driver who has arrived takes a free pod seat himself. The drive there is
// titanPodGoal, from updateGoal.
bool BotManager::titanPodReady(const GameWorld::PlayerInfo& self, physics::Vec3* spot)
{
	if (!m_settings.titanPlay() || m_world.gameMode() != "gpm_ti" || !self.vehicle || m_world.isTitan(self.vehicle)) {
		return false;
	}
	physics::Vec3 nearest;
	spot = spot ? spot : &nearest;
	const std::vector<physics::Vec3> spots = titanLandingSpots(3 - self.team, self.team);
	float best = 1e9f;
	for (const physics::Vec3& s : spots) {
		const float d = horizontal(s - self.position);
		if (d < best) {
			best = d;
			if (spot) {
				*spot = s;
			}
		}
	}
	if (spot && self.position.y > spot->y + 8) {
		return best <= 40; // from the air (a transport's seat): the pod only falls
	}
	return best >= 25 && best <= 130
		&& m_world.lineOfSight(self.position + physics::Vec3 {0, 4, 0}, self.position + physics::Vec3 {0, 300, 0}, self.vehicle);
}

bool BotManager::titanPodGoal(Bot& bot, const GameWorld::PlayerInfo& self, physics::Vec3& goal)
{
	if (!m_settings.titanPlay() || m_world.gameMode() != "gpm_ti" || !self.vehicle || !self.driving || !m_vehicleNavMesh) {
		return false;
	}
	bool pods = false;
	for (const GameWorld::SeatLayout& seat : m_world.vehicleSeats(self.vehicle)) {
		pods = pods || podSeat(seat);
	}
	const std::vector<physics::Vec3> spots = pods ? titanLandingSpots(3 - self.team, self.team) : std::vector<physics::Vec3> {};
	{ static std::map<int, float> last2; if (std::getenv("BF2142_BOT_STATS") && m_time - last2[self.id] > 30 && (last2[self.id] = m_time, true)) { // DIAG
		const world::Object* v = m_world.objects().find(self.vehicle);
		std::fprintf(stderr, "podgoal? %d team %d %s pods %d spots %zu\n", self.id, self.team, v && v->objectTemplate() ? v->objectTemplate()->name().c_str() : "?", pods ? 1 : 0, spots.size()); } }
	if (spots.empty()) {
		return false;
	}
	// A short wait for riders, once a vehicle: 3..12 s, and only while a
	// boarder of the team on foot is within 40 m and a seat is free.
	if (bot.titanWaitFor != self.vehicle) {
		bot.titanWaitFor = self.vehicle;
		bot.titanWaitUntil = m_time + 3 + 9 * random();
	}
	if (m_time < bot.titanWaitUntil) {
		for (const GameWorld::PlayerInfo& p : m_players) {
			if (p.id != self.id && p.team == self.team && p.alive && !p.vehicle && horizontal(p.position - self.position) < 40 && titanRole(p) == 1) {
				goal = self.position;
				return true;
			}
		}
		bot.titanWaitUntil = 0; // no one is coming
	}
	if (bot.titanPodGoalSet && horizontal(bot.titanPodGoalFor - spots.front()) < 20) {
		goal = bot.titanPodGoal;
		if (static std::map<int, float> last; std::getenv("BF2142_BOT_STATS") && m_time - last[self.id] > 20 && (last[self.id] = m_time, true)) { // DIAG
			std::fprintf(stderr, "podgoal %d t %.0f at %.0f %.0f %.0f goal %.0f %.0f %.0f d %.0f ready %d behaviour %s\n", self.id, m_time, self.position.x, self.position.y, self.position.z,
				goal.x, goal.y, goal.z, horizontal(goal - self.position), titanPodReady(self) ? 1 : 0, bot.behaviour.c_str());
		}
		return true;
	}
	// A place on the vehicle mesh 70 m beside a landing spot, under an open sky: the nearest.
	float best = 1e9f;
	for (size_t i = 0; i < spots.size(); i += std::max<size_t>(1, spots.size() / 40)) {
		for (int k = 0; k < 8; ++k) {
			physics::Vec3 p = spots[i] + physics::Vec3 {std::sin(k * 0.7853982f), 0, std::cos(k * 0.7853982f)} * 70.0f;
			p.y = m_world.terrainHeight(p.x, p.z);
			const float d = horizontal(p - self.position);
			if (d < best && m_vehicleNavMesh->findPolygon(ai::Vec3 {p.x, p.y, p.z}, 5.0f) >= 0
				&& m_world.lineOfSight(p + physics::Vec3 {0, 4, 0}, p + physics::Vec3 {0, 300, 0}, self.vehicle)) {
				best = d;
				goal = p;
			}
		}
	}
	if (best >= 1e9f) {
		static int said = 0;
		if (said++ < 3 && std::getenv("BF2142_BOT_STATS")) std::fprintf(stderr, "podgoal %d: no launch place among %zu spots\n", self.id, spots.size()); // DIAG
		return false;
	}
	bot.titanPodGoal = goal;
	bot.titanPodGoalFor = spots.front();
	bot.titanPodGoalSet = true;
	return true;
}

bool BotManager::titanPod(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	if (!m_settings.titanPlay() || m_world.gameMode() != "gpm_ti" || !self.vehicle) {
		return false;
	}
	if (m_world.ridingPod(self.id)) {
		// Over the nearest landing spot: the pod's thrusters against the way still to go, less the way it drifts.
		const std::vector<physics::Vec3> spots = titanLandingSpots(3 - self.team, self.team);
		const world::Object* pod = m_world.objects().find(self.vehicle);
		if (spots.empty() || !pod) {
			return false;
		}
		const physics::Vec3* spot = &spots.front();
		for (const physics::Vec3& s : spots) {
			if (horizontal(s - self.position) < horizontal(*spot - self.position)) {
				spot = &s;
			}
		}
		if (horizontal(*spot - self.position) > 250) {
			return false; // a drop from its own Titan: as before
		}
		const physics::Vec3 want = *spot - self.position - self.velocity * 1.5f;
		const float yaw = pod->rotation().x * 0.017453292f;
		input.throttle = std::clamp((want.x * std::sin(yaw) + want.z * std::cos(yaw)) / 8, -1.0f, 1.0f);
		input.strafe = std::clamp((want.x * std::cos(yaw) - want.z * std::sin(yaw)) / 8, -1.0f, 1.0f);
		if (std::getenv("BF2142_BOT_STATS")) { // DIAG
			std::fprintf(stderr, "pod %d t %.1f at %.0f %.0f %.0f v %.0f %.0f %.0f spot d %.0f dy %.0f yaw %.0f thr %.2f str %.2f\n", self.id, m_time, self.position.x, self.position.y,
				self.position.z, self.velocity.x, self.velocity.y, self.velocity.z, horizontal(*spot - self.position), spot->y - self.position.y, pod->rotation().x, input.throttle, input.strafe);
		}
		bot.behaviour = "TitanPod";
		return true;
	}
	const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(self.vehicle);
	if (self.seat < 0 || static_cast<size_t>(self.seat) >= seats.size()) {
		return false;
	}
	// An air transport (every seat of it launches a pod): with the enemy's
	// shield down its pilot waits 6..18 s on the deck for riders, flies over
	// the middle of the enemy's deck, comes straight down and stays in his
	// seat, so the transport is a spawn point there; the riders leave in
	// their pods once over the deck (the exit key, exitThroughPodLauncher).
	const world::Object* object = m_world.objects().find(self.vehicle);
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	const GameWorld::TitanInfo* enemy = titanOf(titans, 3 - self.team);
	if (enemy && !enemy->shield && podSeat(seats[0]) && object && object->objectTemplate()
		&& (m_settings.typeFlags(aiTemplateOf(object->objectTemplate()->name())) & kITAir)) {
		const std::vector<physics::Vec3> spots = titanLandingSpots(enemy->team, self.team);
		if (spots.empty()) {
			return false;
		}
		physics::Vec3 middle;
		for (const physics::Vec3& sp : spots) {
			middle = middle + sp * (1.0f / spots.size());
		}
		physics::Vec3 spot = spots.front();
		for (const physics::Vec3& sp : spots) {
			spot = horizontal(sp - middle) < horizontal(spot - middle) ? sp : spot;
		}
		const float d = horizontal(spot - self.position);
		const float up = self.position.y - spot.y;
		bot.behaviour = "TitanFly";
		if (!self.driving) {
			input.use = d < 40 && up > -3 && std::fmod(m_time, 1.0f) < 0.1f;
			return true;
		}
		const GameWorld::TitanInfo* home = titanOf(titans, self.team);
		const bool parked = d < 8 && up < 4;
		bot.titanFlyMode = parked ? 3 : 2;
		// Down once over the spot, and on down while within 60 m of it (the hover drifts).
		bot.titanFlyDown = d < 60 && (bot.titanFlyDown || d < 20);
		// (The crossing: each team at its own height, the two transports fly the same line.)
		bot.titanFlyTo = spot + physics::Vec3 {0, bot.titanFlyDown ? 1.5f : d > 150 ? 25.0f + 30 * self.team : 45.0f, 0};
		if (home && aboard(*home, self.position) && !bot.heli.airborne) {
			if (bot.titanWaitFor != self.vehicle) {
				bot.titanWaitFor = self.vehicle;
				bot.titanWaitUntil = m_time + 6 + 12 * random();
			}
			const std::vector<int> riders = m_world.vehicleOccupants(self.vehicle);
			const auto free = std::count_if(riders.begin(), riders.end(), [](int r) { return r < 0; });
			bool coming = false;
			for (const GameWorld::PlayerInfo& p : m_players) {
				coming = coming || (p.id != self.id && p.team == self.team && p.alive && !p.vehicle && aboard(*home, p.position) && titanRole(p) == 1);
			}
			if (m_time < bot.titanWaitUntil && coming && free > 2) {
				bot.titanFlyMode = 1;
			}
		}
		if (static std::map<int, float> last; std::getenv("BF2142_BOT_STATS") && m_time - last[self.id] > 5 && (last[self.id] = m_time, true)) { // DIAG
			std::fprintf(stderr, "fly %d t %.0f mode %d at %.0f %.0f %.0f spot %.0f %.0f %.0f d %.0f up %.0f\n", self.id, m_time, bot.titanFlyMode, self.position.x, self.position.y,
				self.position.z, spot.x, spot.y, spot.z, d, up);
		}
		return fly(bot, self, input, seconds);
	}
	if (titanSortie(bot, self, input, seconds)) {
		return true;
	}
	if (!titanPodReady(self)) {
		return false;
	}
	if (podSeat(seats[static_cast<size_t>(self.seat)])) {
		input.altFire = std::fmod(m_time, 1.0f) < 0.5f; // pod_launcher's fire.fireInput PIAltFire
		bot.behaviour = "TitanPod";
		return true;
	}
	if (self.driving && physics::lengthSquared(self.velocity) < 3 * 3) {
		const std::vector<int> riders = m_world.vehicleOccupants(self.vehicle);
		for (size_t i = 0; i < seats.size(); ++i) {
			if (podSeat(seats[i]) && (i >= riders.size() || riders[i] < 0)) {
				input.switchSeat = static_cast<int>(i) + 1;
				bot.behaviour = "TitanPod";
				return true;
			}
		}
	}
	return false;
}

// The deck aircraft of its own Titan for a bot on foot aboard it, or null.
// In order: a pilot for the gunship, then for the transport; the gunship's
// gunner once its pilot sits; riders for the transport while its pilot
// waits on the deck. Each seat is claimed by one bot walking to it (the
// claim it has kept first).
const GameWorld::VehicleInfo* BotManager::titanAircraft(const GameWorld::PlayerInfo& self, const GameWorld::TitanInfo& home)
{
	const Bot& me = m_bots[self.id];
	const GameWorld::VehicleInfo* best = nullptr;
	int bestRank = 4;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		const world::Object* o = m_world.objects().find(v.object);
		if (!v.working || v.freeSeats <= 0 || !aboard(home, v.position) || !o || !o->objectTemplate() || (o->team() != 0 && o->team() != self.team)
			|| !(m_settings.typeFlags(aiTemplateOf(o->objectTemplate()->name())) & kITAir)
			|| (me.titanCrewSkipFor == v.object && m_time < me.titanCrewSkipUntil)) {
			continue;
		}
		const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(v.object);
		if (seats.empty()) {
			continue;
		}
		const bool transport = podSeat(seats[0]);
		const std::vector<int> occupants = m_world.vehicleOccupants(v.object);
		const int pilot = occupants.empty() ? -1 : occupants[0];
		const bool parked = std::sqrt(physics::lengthSquared(v.velocity - home.velocity)) < 2;
		int claims = 0;
		for (const auto& [id, other] : m_bots) {
			const GameWorld::PlayerInfo* mate = id == self.id || other.titanCrew != v.object ? nullptr : playerOf(id);
			claims += mate && mate->team == self.team && mate->alive && !mate->vehicle;
		}
		int rank = 4, wanted = 0;
		if (pilot < 0) {
			// Its pilot first; then, while the pilot is still on the way,
			// the gunner or the riders.
			rank = (transport ? 1 : 0) + (claims > 0 ? 2 : 0);
			wanted = v.freeSeats;
		} else if (!parked) {
			continue;
		} else if (!transport) {
			rank = 2;
			wanted = 1;
		} else {
			const auto flier = m_bots.find(pilot);
			if (flier == m_bots.end() || flier->second.titanSortie != 0) {
				continue;
			}
			rank = 3;
			wanted = v.freeSeats;
		}
		if (claims >= wanted) {
			continue;
		}
		if (me.titanCrew == v.object) {
			return &v;
		}
		if (rank < bestRank) {
			bestRank = rank;
			best = &v;
		}
	}
	return best;
}

// A bot on foot aboard its own Titan, past the guns' share: to the deck
// aircraft it crews, over the Titan's grid, and in (the use key beside it;
// the first free seat is the pilot's while no one flies it). Given up after
// a minute on the way, that aircraft not tried again for two.
bool BotManager::titanCrew(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	if (!m_settings.titanPlay() || m_world.gameMode() != "gpm_ti" || self.vehicle) {
		bot.titanCrew = 0;
		return false;
	}
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	const GameWorld::TitanInfo* home = titanOf(titans, self.team);
	const GameWorld::VehicleInfo* v = home && aboard(*home, self.position) ? titanAircraft(self, *home) : nullptr;
	if (!v) {
		bot.titanCrew = 0;
		return false;
	}
	if (bot.titanCrew != v->object) {
		bot.titanCrew = v->object;
		bot.titanCrewSince = m_time;
	} else if (m_time - bot.titanCrewSince > 60) {
		bot.titanCrewSkipFor = v->object;
		bot.titanCrewSkipUntil = m_time + 120;
		bot.titanCrew = 0;
		return false;
	}
	TitanGrid& g = titanGrid(*home, self.team);
	bot.behaviour = "TitanCrew";
	// Its authored entry point (entry is within the point's radius of the
	// body, which on a raised pad the centre is too high or far to give).
	physics::Vec3 entry = v->position;
	m_world.entryPoint(v->object, entry);
	// Around the aircraft to an entry off its middle (the gunships' is 5 m
	// behind it), straight to one at its middle (the transports' cabin).
	const bool outside = horizontal(entry - v->position) > 3;
	const physics::Vec3 d = entry - self.position;
	input.use = (physics::lengthSquared(d) < 9 || (horizontal(d) < 7 && std::fabs(d.y) < 5)) && std::fmod(m_time, 0.6f) < 0.1f;
	titanWalk(bot, self, input, seconds, g, *home,
		titanField(g, *home, self.team, 0x20000000u ^ v->object, m_world.titanToLocal(home->object, entry), true, 0, outside ? v->object : 0));
	return true;
}

// A transport of its own Titan's deck while the enemy's shield is up (with
// it down, titanPod's boarding run): its pilot waits 6..18 s on the deck for
// riders (on while one of them is still walking to it, 20 s at most), with
// one aboard flies to the nearest silo the team doesn't hold, high on the
// way and 30 m over it, waits there until the riders are out (15 s at most)
// and lands back on its Titan's deck. Its riders leave by pod (the exit key) over
// such a silo; with no pilot, at once.
bool BotManager::titanSortie(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	const world::Object* object = m_world.objects().find(self.vehicle);
	const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(self.vehicle);
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	const GameWorld::TitanInfo* home = titanOf(titans, self.team);
	if (!home || !object || !object->objectTemplate() || seats.empty()
		|| !(m_settings.typeFlags(aiTemplateOf(object->objectTemplate()->name())) & kITAir)) {
		bot.titanSortie = 0;
		return false;
	}
	const std::vector<int> occupants = m_world.vehicleOccupants(self.vehicle);
	const std::vector<GameWorld::SiloInfo> silos = m_world.missileSilos();
	if (!podSeat(seats[0])) {
		// A gunship from the deck: off it and 200 m clear of the Titan (high,
		// toward the nearest silo the team doesn't hold, else ahead of the
		// Titan), then the ordinary pilot's. On the deck that one waits
		// while soldiers are near, and the Titan's crew always is.
		bot.titanSortie = self.driving && horizontal(self.position - home->position) < 200
			&& (bot.titanSortie == 1 || aboard(*home, self.position)) ? 1 : 0;
		if (!bot.titanSortie) {
			return false;
		}
		const GameWorld::SiloInfo* best = nullptr;
		for (const GameWorld::SiloInfo& silo : silos) {
			if (silo.team != self.team && (!best || horizontal(silo.position - self.position) < horizontal(best->position - self.position))) {
				best = &silo;
			}
		}
		const physics::Vec3 to = best ? best->position : m_world.titanToWorld(home->object, {0, 0, 400});
		bot.behaviour = "TitanTakeoff";
		bot.titanFlyTo = {to.x, std::max(m_world.terrainHeight(to.x, to.z) + 70, m_world.titanToWorld(home->object, {0, 40, 0}).y), to.z};
		bot.titanFlyMode = 4;
		return fly(bot, self, input, seconds);
	}
	const bool onDeck = aboard(*home, self.position);
	if (!self.driving) {
		const bool pilot = !occupants.empty() && occupants[0] >= 0;
		if (onDeck && pilot) {
			bot.behaviour = "TitanSortie";
			return true; // seated until it is off
		}
		if (onDeck) {
			return false;
		}
		bool over = !pilot;
		for (const GameWorld::SiloInfo& silo : silos) {
			over = over || (silo.team != self.team && horizontal(silo.position - self.position) < 40
				&& self.position.y - m_world.terrainHeight(self.position.x, self.position.z) > 12);
		}
		bot.behaviour = "TitanSortie";
		input.use = over && std::fmod(m_time, 1.0f) < 0.1f;
		return true;
	}
	const auto riders = std::count_if(occupants.begin() + 1, occupants.end(), [](int r) { return r >= 0; });
	const physics::Vec3 relative = self.velocity - home->velocity;
	if (bot.titanWaitFor != self.vehicle) {
		bot.titanWaitFor = self.vehicle;
		bot.titanWaitUntil = m_time + 6 + 12 * random();
		bot.titanSortie = onDeck ? 0 : 2;
		if (onDeck) {
			bot.titanPad = m_world.titanToLocal(home->object, self.position);
		}
	}
	const auto siloOf = [&](int point) -> const GameWorld::SiloInfo* {
		for (const GameWorld::SiloInfo& silo : silos) {
			if (silo.controlPoint == point && silo.team != self.team) {
				return &silo;
			}
		}
		return nullptr;
	};
	const auto nearestSilo = [&]() -> const GameWorld::SiloInfo* {
		const GameWorld::SiloInfo* best = nullptr;
		for (const GameWorld::SiloInfo& silo : silos) {
			if (silo.team != self.team && (!best || horizontal(silo.position - self.position) < horizontal(best->position - self.position))) {
				best = &silo;
			}
		}
		return best;
	};
	bot.behaviour = "TitanSortie";
	if (bot.titanSortie == 0) {
		bool coming = false;
		for (const auto& [id, other] : m_bots) {
			const GameWorld::PlayerInfo* mate = id == self.id || other.titanCrew != self.vehicle ? nullptr : playerOf(id);
			coming = coming || (mate && mate->team == self.team && mate->alive && !mate->vehicle);
		}
		const int free = static_cast<int>(std::count(occupants.begin(), occupants.end(), -1)) + static_cast<int>(seats.size() - std::min(seats.size(), occupants.size()));
		const bool go = riders > 0 && (free == 0 || m_time >= bot.titanWaitUntil + (coming ? 20 : 0));
		const GameWorld::SiloInfo* silo = go ? nearestSilo() : nullptr;
		if (!silo) {
			if (onDeck) {
				bot.titanFlyMode = 1;
			} else {
				bot.titanSortie = 2;
			}
		} else {
			bot.titanSortie = 1;
			bot.titanSortieSilo = silo->controlPoint;
			bot.titanSortieUntil = 0;
		}
	}
	if (bot.titanSortie == 1) {
		const GameWorld::SiloInfo* silo = siloOf(bot.titanSortieSilo);
		if (!silo && riders > 0 && (silo = nearestSilo())) {
			bot.titanSortieSilo = silo->controlPoint;
		}
		if (!silo) {
			bot.titanSortie = 2;
		} else {
			const float d = horizontal(silo->position - self.position);
			if (d < 40 && bot.titanSortieUntil <= 0) {
				bot.titanSortieUntil = m_time + 15;
			}
			if (bot.titanSortieUntil > 0 && (riders == 0 || m_time > bot.titanSortieUntil)) {
				bot.titanSortie = 2;
				bot.titanFlyDown = false;
			} else {
				// Over the Titan until clear of it: below its deck the pilot
				// only presses down onto it.
				const float clear = horizontal(self.position - home->position) < 200 ? m_world.titanToWorld(home->object, {0, 40, 0}).y : 0.0f;
				bot.titanFlyTo = {silo->position.x,
					std::max(clear, m_world.terrainHeight(silo->position.x, silo->position.z) + (d > 150 ? 70.0f : 30.0f)), silo->position.z};
				bot.titanFlyMode = 4;
			}
		}
	}
	if (bot.titanSortie == 2) {
		const physics::Vec3 pad = m_world.titanToWorld(home->object, bot.titanPad);
		const float d = horizontal(pad - self.position);
		const float up = self.position.y - pad.y;
		// Down anywhere on the deck (the hover drifts as it sinks) is home:
		// that spot its pad from now on.
		if ((d < 8 || onDeck) && up < 4 && physics::lengthSquared(relative) < 4) {
			bot.titanPad = m_world.titanToLocal(home->object, self.position);
			bot.titanSortie = 0;
			bot.titanWaitUntil = m_time + 6 + 12 * random();
			bot.titanFlyMode = 3;
		} else {
			// Down once over the pad, and on down while within 60 m of it (the hover drifts).
			bot.titanFlyDown = d < 60 && (bot.titanFlyDown || d < 20);
			bot.titanFlyTo = pad + physics::Vec3 {0, bot.titanFlyDown ? 1.5f : d > 150 ? 25.0f + 30 * self.team : 45.0f, 0};
			bot.titanFlyMode = d > 150 ? 4 : 2;
		}
	}
	if (static std::map<int, float> last; std::getenv("BF2142_BOT_STATS") && m_time - last[self.id] > 5 && (last[self.id] = m_time, true)) { // DIAG
		std::fprintf(stderr, "sortie %d t %.0f state %d mode %d riders %ld at %.0f %.0f %.0f to %.0f %.0f %.0f\n", self.id, m_time, bot.titanSortie, bot.titanFlyMode,
			static_cast<long>(riders), self.position.x, self.position.y, self.position.z, bot.titanFlyTo.x, bot.titanFlyTo.y, bot.titanFlyTo.z);
	}
	return fly(bot, self, input, seconds);
}

// The team's plan, made anew every 5 s (the virtual commander). Squads
// keep their role while it is still wanted; the defenders are the squads
// nearest their Titan, the boarders the nearest to the enemy's.
//   - defend: one squad for every two enemies aboard; one while the
//     shield is down;
//   - board: with the enemy's shield down, 40..70 % of the rest (the
//     share is drawn when the shield goes down), at least one;
//   - the others: the silos, as before.
int BotManager::titanRole(const GameWorld::PlayerInfo& self)
{
	if (!m_settings.titanPlay() || m_world.gameMode() != "gpm_ti" || self.team < 1 || self.team > 2) {
		return 0;
	}
	TitanPlan& plan = m_titanPlans[self.team];
	const auto keyOf = [](const GameWorld::PlayerInfo& p) { return p.squad ? p.squad : 100 + p.id; };
	if (m_time >= plan.nextAt) {
		plan.nextAt = m_time + 5;
		const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
		const GameWorld::TitanInfo* own = titanOf(titans, self.team);
		const GameWorld::TitanInfo* enemy = titanOf(titans, 3 - self.team);
		const std::map<int, int> old = std::move(plan.roles);
		plan.roles.clear();
		if (own && enemy) {
			int boarders = 0;
			std::map<int, physics::Vec3> groups; // where a member of each is
			for (const GameWorld::PlayerInfo& p : m_players) {
				boarders += p.alive && p.team == enemy->team && !p.vehicle && aboard(*own, p.position);
				if (p.team == self.team && p.ai) {
					groups[keyOf(p)] = p.position;
				}
			}
			const bool down = !enemy->shield;
			if (down && !plan.wasDown) {
				plan.share = 0.4f + 0.3f * random();
			}
			plan.wasDown = down;
			const int count = static_cast<int>(groups.size());
			const int defend = std::min(count, boarders ? (boarders + 1) / 2 : (own->shield ? 0 : 1));
			const int board = down ? std::max(1, static_cast<int>(std::lround(plan.share * (count - defend)))) : 0;
			const auto give = [&](int role, int wanted, const physics::Vec3& by) {
				std::vector<std::pair<float, int>> order;
				for (const auto& [key, at] : groups) {
					const auto was = old.find(key);
					if (!plan.roles.count(key)) {
						order.push_back({was != old.end() && was->second == role ? -1.0f : horizontal(at - by), key});
					}
				}
				std::sort(order.begin(), order.end());
				for (int i = 0; i < wanted && static_cast<size_t>(i) < order.size(); ++i) {
					plan.roles[order[static_cast<size_t>(i)].second] = role;
				}
			};
			give(2, defend, own->position);
			give(1, board, enemy->position);
			if (std::getenv("BF2142_BOT_STATS") && plan.roles != old) { // DIAG
				std::fprintf(stderr, "titan plan t %.0f team %d: %d groups, %d enemies aboard, enemy shield %s:", m_time, self.team, count, boarders, down ? "down" : "up");
				for (const auto& [key, role] : plan.roles) {
					std::fprintf(stderr, " %d=%s", key, role == 1 ? "board" : "defend");
				}
				std::fprintf(stderr, "\n");
			}
		}
	}
	const auto role = plan.roles.find(keyOf(self));
	return role == plan.roles.end() ? 0 : role->second;
}

// The team's air transport on its Titan's deck with a seat free.
const GameWorld::VehicleInfo* BotManager::titanTransport(int team, const GameWorld::TitanInfo& home)
{
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		const world::Object* o = m_world.objects().find(v.object);
		if (!v.working || v.freeSeats <= 0 || !aboard(home, v.position) || !o || !o->objectTemplate() || (o->team() != 0 && o->team() != team)
			|| !(m_settings.typeFlags(aiTemplateOf(o->objectTemplate()->name())) & kITAir)) {
			continue;
		}
		const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(v.object);
		if (!seats.empty() && podSeat(seats[0])) {
			return &v;
		}
	}
	return nullptr;
}

// Where a bot with a role spawns: a defender on its Titan; a boarder in a
// spawn vehicle of the team within 200 m of the enemy's Titan (a transport
// parked on its deck, an APC beside it), else on its own Titan while the
// transport there has room. False: the SAI's choice stands.
bool BotManager::titanRespawn(const GameWorld::PlayerInfo& self)
{
	const int role = titanRole(self);
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	const GameWorld::TitanInfo* own = titanOf(titans, self.team);
	const GameWorld::TitanInfo* enemy = titanOf(titans, 3 - self.team);
	if (!role || !own || !enemy) {
		return false;
	}
	if (role == 1) {
		if (enemy->shield) {
			return false;
		}
		for (uint32_t vehicle : m_world.spawnVehicles(self.team)) {
			for (const GameWorld::VehicleInfo& v : m_vehicles) {
				if (v.object == vehicle && v.working && v.freeSeats > 0 && horizontal(v.position - enemy->position) < 200) {
					m_world.setSpawnVehicle(self.id, vehicle);
					return true;
				}
			}
		}
		if (!titanTransport(self.team, *own)) {
			return false;
		}
	}
	m_world.setSpawnTitan(self.id, own->object);
	return true;
}

// A boarder on foot on the ground: to a vehicle within 120 m with a free
// pod seat, its own team's or no one's (an APC left beside the enemy's
// Titan, whoever brought it; one a teammate drives within 40 m).
bool BotManager::titanRideGoal(Bot& bot, const GameWorld::PlayerInfo& self, physics::Vec3& goal)
{
	if (self.vehicle || titanRole(self) != 1) {
		return false;
	}
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	const GameWorld::TitanInfo* enemy = titanOf(titans, 3 - self.team);
	if (!enemy || enemy->shield || std::any_of(titans.begin(), titans.end(), [&](const auto& t) { return aboard(t, self.position); })) {
		return false;
	}
	if (m_time - bot.titanRideAt > 2) {
		bot.titanRideAt = m_time;
		bot.titanRide = 0;
		const std::vector<physics::Vec3> spots = titanLandingSpots(enemy->team, self.team);
		float best = 120;
		for (const GameWorld::VehicleInfo& v : m_vehicles) {
			const world::Object* o = m_world.objects().find(v.object);
			const float d = horizontal(v.position - self.position);
			if (!v.working || d >= best || !o || (o->team() != 0 && o->team() != self.team) || aboard(*enemy, v.position)) {
				continue;
			}
			const std::vector<GameWorld::SeatLayout> seats = m_world.vehicleSeats(v.object);
			const std::vector<int> riders = m_world.vehicleOccupants(v.object);
			bool seat = false;
			for (size_t i = 0; i < seats.size(); ++i) {
				seat = seat || (podSeat(seats[i]) && (i >= riders.size() || riders[i] < 0));
			}
			float toSpot = 1e9f;
			for (const physics::Vec3& sp : spots) {
				toSpot = std::min(toSpot, horizontal(sp - v.position));
			}
			if (seat && ((toSpot >= 25 && toSpot <= 130) || (!v.driverFree && d < 40))) {
				best = d;
				bot.titanRide = v.object;
			}
		}
	}
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		if (bot.titanRide && v.object == bot.titanRide) {
			goal = v.position;
			return true;
		}
	}
	return false;
}

// The use key beside that vehicle.
void BotManager::titanRide(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input)
{
	if (self.vehicle || !bot.titanRide || m_time - bot.titanRideAt > 3) {
		return;
	}
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		if (v.object == bot.titanRide && horizontal(v.position - self.position) < 6) {
			input.use = !bot.usedLast;
		}
	}
}

const std::vector<physics::Vec3>& BotManager::wayToVehicleMesh(const physics::Vec3& from, uint32_t ignore)
{
	constexpr float kGrid = 4;
	constexpr int kHalf = 22; // 88 m
	const std::pair<int, int> key {static_cast<int>(std::floor(from.x / kGrid)), static_cast<int>(std::floor(from.z / kGrid))};
	const auto known = m_meshWays.find(key);
	if (known != m_meshWays.end() && (!known->second.second.empty() || m_time - known->second.first < 10)) {
		return known->second.second;
	}
	m_meshWays[key].first = m_time;
	std::vector<physics::Vec3>& way = m_meshWays[key].second;
	way.clear();
	if (!m_vehicleNavMesh) {
		return way;
	}
	const int side = 2 * kHalf + 1;
	const auto at = [&](int i) {
		physics::Vec3 p {from.x + (i % side - kHalf) * kGrid, 0, from.z + (i / side - kHalf) * kGrid};
		p.y = m_world.terrainHeight(p.x, p.z);
		return p;
	};
	// A step is open when nothing crosses it at 1 m and 1.8 m, on its line and 1.5 m to either side (a hull's width).
	const auto open = [&](const physics::Vec3& a, const physics::Vec3& b) {
		const physics::Vec3 d = b - a;
		const float length = horizontal(d);
		const physics::Vec3 across = physics::Vec3 {d.z, 0, -d.x} * (1.5f / std::max(length, 0.01f));
		for (const float aside : {0.0f, 1.0f, -1.0f}) {
			for (const float up : {1.0f, 1.8f}) {
				bool through = false;
				const physics::Vec3 lift = across * aside + physics::Vec3 {0, up, 0};
				if (!m_world.lineOfSight(a + lift, b + lift, ignore, through) || through) {
					return false;
				}
			}
		}
		return std::fabs(d.y) < 0.6f * length; // not up a wall of terrain
	};
	std::vector<int> cameFrom(static_cast<size_t>(side * side), -1);
	std::queue<int> frontier;
	const int start = kHalf * side + kHalf;
	cameFrom[static_cast<size_t>(start)] = start;
	frontier.push(start);
	int goal = -1;
	while (!frontier.empty() && goal < 0) {
		const int c = frontier.front();
		frontier.pop();
		const physics::Vec3 here = c == start ? physics::Vec3 {from.x, m_world.terrainHeight(from.x, from.z), from.z} : at(c);
		for (int dz = -1; dz <= 1 && goal < 0; ++dz) {
			for (int dx = -1; dx <= 1; ++dx) {
				const int x = c % side + dx, z = c / side + dz, n = z * side + x;
				if ((!dx && !dz) || x < 0 || z < 0 || x >= side || z >= side || cameFrom[static_cast<size_t>(n)] >= 0 || !open(here, at(n))) {
					continue;
				}
				cameFrom[static_cast<size_t>(n)] = c;
				const physics::Vec3 p = at(n);
				if (m_vehicleNavMesh->findPolygon(ai::Vec3 {p.x, p.y, p.z}) >= 0) {
					goal = n;
					break;
				}
				frontier.push(n);
			}
		}
	}
	for (int c = goal; c >= 0 && c != start; c = cameFrom[static_cast<size_t>(c)]) {
		way.insert(way.begin(), at(c));
	}
	if (std::getenv("BF2142_BOT_STATS")) { // DIAG
		std::fprintf(stderr, "mesh way from %.0f %.0f: %zu points%s", from.x, from.z, way.size(), way.empty() ? "\n" : ", to ");
		if (!way.empty()) std::fprintf(stderr, "%.0f %.0f\n", way.back().x, way.back().z);
	}
	return way;
}

bool BotManager::driveToMesh(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input)
{
	if (!m_settings.stuckRecovery() || !self.vehicle || !self.driving || !m_vehicleNavMesh || m_world.isTitan(self.vehicle)) {
		return false;
	}
	if (bot.meshVehicle != self.vehicle) {
		bot.meshVehicle = self.vehicle;
		bot.meshReached = false;
		bot.meshWay.clear();
		bot.meshSince = m_time;
	}
	const world::Object* object = m_world.objects().find(self.vehicle);
	const GameWorld::VehicleInfo* vehicle = nullptr;
	for (const GameWorld::VehicleInfo& v : m_vehicles) {
		vehicle = v.object == self.vehicle ? &v : vehicle;
	}
	// Only from where it was parked: once on the mesh the drivers' own planning has it, off its edge too.
	if (bot.meshReached || !vehicle || !object || !object->objectTemplate()
		|| (m_settings.typeFlags(aiTemplateOf(object->objectTemplate()->name())) & kITAir)
		|| !m_settings.plugin(aiTemplateOf(object->objectTemplate()->name()), "Mobile")) {
		return false;
	}
	const physics::Vec3 here = vehicle->position;
	if (m_vehicleNavMesh->findPolygon(ai::Vec3 {here.x, here.y, here.z}) >= 0) {
		bot.meshReached = true;
		return false;
	}
	if (m_time - bot.meshSince > 60) {
		// shortcut: the grid search knows no hull's turning room; a vehicle it cannot bring out is given up
		m_meshGaveUp.insert(self.vehicle);
		input.use = std::fmod(m_time, 1.0f) < 0.1f;
		bot.behaviour = "MoveTo (to the mesh)";
		return true;
	}
	// Its way, kept while it makes way; looked up again from where it stands after 6 s without 3 m made.
	if (horizontal(here - bot.meshAt) > 3) {
		bot.meshAt = here;
		bot.meshAtTime = m_time;
	}
	if (bot.meshWay.empty() || m_time - bot.meshAtTime > 6) {
		if (const std::vector<physics::Vec3>& way = wayToVehicleMesh(here, self.vehicle); !way.empty()) {
			bot.meshWay = way;
		}
		if (m_time - bot.meshAtTime > 6) {
			bot.meshBackUntil = m_time + 2.5f; // wedged against something: back off first
		}
		bot.meshAtTime = m_time;
	}
	while (bot.meshWay.size() > 1 && horizontal(bot.meshWay.front() - here) < 5) {
		bot.meshWay.erase(bot.meshWay.begin());
	}
	if (bot.meshWay.empty()) {
		return false;
	}
	const physics::Vec3 d = bot.meshWay.front() - here;
	const float off = std::remainder((std::atan2(d.x, d.z) - std::atan2(vehicle->forward.x, vehicle->forward.z)) * 57.29578f, 360.0f);
	if (m_settings.moveInterpreter(equipmentOf(self)) == "FloatingTankMoveTo") {
		// The hover tank slides: PIYaw is sideways on it, so it goes straight at the point as it stands.
		input.throttle = 0.7f * std::cos(off * 0.017453292f);
		input.strafe = 0.7f * std::sin(off * 0.017453292f);
	} else {
		input.strafe = std::clamp(off / 30, -1.0f, 1.0f);
		input.throttle = std::fabs(off) < 60 ? 0.7f : 0.3f;
	}
	if (m_time < bot.meshBackUntil) {
		input.throttle = -0.7f;
		input.strafe = -input.strafe;
	}
	bot.behaviour = "MoveTo (to the mesh)";
	return true;
}

std::string BotManager::titanGridStats()
{
	std::string out;
	for (const auto& [key, g] : m_titanGrids) {
		size_t links = 0;
		for (const auto& c : g->cells) {
			links += c.out.size();
		}
		out += "  titan grid: object " + std::to_string(key.first) + " team " + std::to_string(key.second) + ", " + std::to_string(g->cells.size())
			+ " cells, " + std::to_string(links) + " links, " + std::to_string(g->fields.size()) + " fields\n";
	}
	for (const GameWorld::VehicleInfo& v : m_vehicles) { // DIAG
		const world::Object* o = m_world.objects().find(v.object);
		const std::string name = o && o->objectTemplate() ? o->objectTemplate()->name() : "?";
		{
			char line[200];
			std::snprintf(line, sizeof line, "  vehicle %s team %d at %.0f %.0f %.0f free %d driverFree %d working %d\n", name.c_str(), o->team(), v.position.x, v.position.y, v.position.z,
				v.freeSeats, v.driverFree ? 1 : 0, v.working ? 1 : 0);
			out += line;
		}
	}
	return out;
}

} // namespace dice::hfe::game
