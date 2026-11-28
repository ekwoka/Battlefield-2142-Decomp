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
#include "BotWorkers.h"
#include "TickProf.h"

#include "Physics/MaterialManager.h"
#include "World/Object.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <queue>
#include <set>

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

// titanField's arguments, kept with the field (enhanced.botAsyncPaths: made anew with them for a new grid).
struct FieldSpec {
	physics::Vec3 point;
	bool nearestOnly = false;
	float entryRadius = 0;
	uint32_t around = 0;
};

// What making a field needs from the world, taken on the main thread: the part's aim point and the cells under
// `around` (FieldSpec), so that makeField itself reads only the grid and its two ray queries.
struct FieldQuery {
	uint32_t part = 0;
	physics::Vec3 point;
	bool nearestOnly = false;
	float entryRadius = 0;
	bool aimed = false;
	physics::Vec3 aim;
	std::vector<char> blocked; // by cell; empty: none
};

// titanField's making: `shot(from, to)` is GameWorld::titanLocalShot's, `ray(from, to, fraction)` titanLocalRay's
// for the grid's team (the live world's, or a TitanCollision copy's off the main thread).
template <typename Grid, typename Shot, typename Ray>
void makeField(const Grid& g, const FieldQuery& q, const Shot& shot, const Ray& ray, std::vector<float>& field)
{
	field.assign(g.cells.size(), kInfinity);
	const auto blocked = [&](size_t c) { return !q.blocked.empty() && q.blocked[c] != 0; };
	using Entry = std::pair<float, int>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
	float nearest = 1e9f;
	int nearestCell = -1;
	const physics::Vec3& point = q.point;
	const physics::Vec3& aim = q.aim;
	const bool aimed = q.aimed;
	// A part none of whose spots meets that test (the core, walled in by the hull) is shot from any distance its rounds reach it.
	for (int pass = 0; pass < (aimed ? 2 : 1) && open.empty(); ++pass) {
		const bool shooting = aimed && pass == 0;
		for (size_t c = 0; c < g.cells.size(); ++c) {
			const physics::Vec3 eye = g.cells[c].at + physics::Vec3 {0, 1.5f, 0};
			const float d = std::sqrt(physics::lengthSquared(point - eye));
			if (blocked(c)) {
				continue;
			}
			if (d < nearest && !g.cells[c].out.empty()) {
				nearest = d;
				nearestCell = static_cast<int>(c);
			}
			// Leave room for cellAt's snap: reaching a terminal cell must also
			// put the actual soldier inside the interaction sphere.
			const float reach = std::max(0.0f, q.entryRadius - kCell);
			const bool inEntry = q.entryRadius > 0
				&& physics::lengthSquared(point - (g.cells[c].at + physics::Vec3 {0, 1, 0})) <= reach * reach;
			if (inEntry) {
				field[c] = 0;
				open.push({0, static_cast<int>(c)});
				continue;
			}
			if (q.nearestOnly) {
				continue; // (no spot that sees the point: the rays below are for the other fields)
			}
			float f = 1;
			const float toAim = shooting ? std::sqrt(physics::lengthSquared(aim - eye)) : 0.0f;
			// Pass 2 (a part walled in by the hull, the core): any distance its rounds reach it from.
			const float nearLimit = pass == 0 ? kFireNear : 0.0f;
			const float toAim2 = std::sqrt(physics::lengthSquared(aim - eye));
			const bool sees = aimed && pass == 1 ? toAim2 >= 1.0f && toAim2 <= kFireFar
					&& shot(eye, aim + (aim - eye) * (0.5f / toAim2)) == q.part
				: shooting ? toAim >= nearLimit && toAim <= kFireFar && shot(eye, aim + (aim - eye) * (0.5f / toAim)) == q.part
						   : d >= kFireNear && d <= kFireFar && (!ray(eye, point, f) || f * d > d - 1.0f);
			if (sees) {
				field[c] = 0;
				open.push({0, static_cast<int>(c)});
			}
		}
	}
	if (open.empty() && nearestCell >= 0 && q.entryRadius <= 0) {
		field[static_cast<size_t>(nearestCell)] = 0;
		open.push({0, nearestCell});
	}
	while (!open.empty()) {
		const auto [cost, c] = open.top();
		open.pop();
		if (cost > field[static_cast<size_t>(c)] || (blocked(static_cast<size_t>(c)) && cost > 0)) {
			continue;
		}
		for (const auto& [from, step] : g.cells[static_cast<size_t>(c)].in) {
			if (cost + step < field[static_cast<size_t>(from)]) {
				field[static_cast<size_t>(from)] = cost + step;
				open.push({cost + step, from});
			}
		}
	}
}

template <typename Grid>
FieldQuery fieldQuery(const GameWorld& world, const Grid& g, const GameWorld::TitanInfo& titan, uint32_t part, const FieldSpec& spec)
{
	FieldQuery q;
	q.part = part;
	q.point = spec.point;
	q.nearestOnly = spec.nearestOnly;
	q.entryRadius = spec.entryRadius;
	if (physics::Vec3 low, high; spec.around && world.objectBox(spec.around, low, high)) {
		q.blocked.assign(g.cells.size(), 0);
		for (size_t c = 0; c < g.cells.size(); ++c) {
			const physics::Vec3 at = world.titanToLocal(spec.around, world.titanToWorld(titan.object, g.cells[c].at + physics::Vec3 {0, 1, 0}));
			q.blocked[c] = at.x > low.x - 0.5f && at.x < high.x + 0.5f && at.z > low.z - 0.5f && at.z < high.z + 0.5f
				&& at.y > low.y - 2 && at.y < high.y + 1;
		}
	}
	// A Titan part's spots are those its rounds reach: the first face of the
	// projectile LOD on the way to the middle of its faces is its own (not a
	// window's glass, a frame or the desk in front of it).
	q.aimed = !spec.nearestOnly && spec.entryRadius <= 0 && world.titanPartAimPoint(part, q.aim);
	return q;
}

// titanLandingSpots' cells: under the open sky (`ray` up clear), clear all round, from which a whole console (or
// the core) can be walked to (`best`, the least of their fields), four cells (3 m) in from any edge or wall.
template <typename Grid, typename Ray>
std::vector<int> makeLanding(const Grid& g, const std::vector<float>& best, const Ray& ray)
{
	std::vector<int> landing;
	for (size_t c = 0; c < g.cells.size(); ++c) {
		float f = 1;
		const physics::Vec3 at = g.cells[c].at;
		if (best[c] < kInfinity && g.cells[c].out.size() >= 8 && !ray(at + physics::Vec3 {0, 0.4f, 0}, physics::Vec3 {at.x, kTop, at.z}, f)) {
			landing.push_back(static_cast<int>(c));
		}
	}
	// A pod comes down fast and wide.
	for (int pass = 0; pass < 4; ++pass) {
		std::vector<char> in(g.cells.size(), 0);
		for (int c : landing) {
			in[static_cast<size_t>(c)] = 1;
		}
		std::vector<int> kept;
		for (int c : landing) {
			const auto& out = g.cells[static_cast<size_t>(c)].out;
			if (std::all_of(out.begin(), out.end(), [&](const auto& link) { return in[static_cast<size_t>(link.first)] != 0; })) {
				kept.push_back(c);
			}
		}
		landing.swap(kept);
	}
	return landing;
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
	std::map<uint32_t, std::pair<physics::Vec3, float>> seeds; // a moving thing's field (a boarder, a deck aircraft): where it was (Titan frame) and when it was made
	// titanRepair's, by part: its armour's material and its meshes (the Titan's frame; titanPartMeshes).
	std::map<uint32_t, std::pair<uint32_t, std::vector<physics::Vec3>>> repairParts;
	// enhanced.botAsyncPaths: each field's making (titanField's arguments), and the fields and the
	// landing cells being made anew off the main thread, taken at `applyAt` (applyTitanJobs). Until
	// then the bots walk the last grid's fields, carried over to this grid's cells (carryTitanFields).
	// The cells, columns and links are never changed once the grid is built: the jobs read them.
	std::map<uint32_t, FieldSpec> specs;
	struct Pending {
		std::shared_ptr<BotWorkers::Job> job;
		std::shared_ptr<std::vector<float>> field;
		std::shared_ptr<std::vector<int>> landing;
		long applyAt = 0;
	};
	std::map<uint32_t, Pending> pending;
	Pending landingJob;
	bool landingStale = false; // carried over: made anew once the fields are
	// The next grid being made (a part went down or came back), and this one's cells in the last (made anew
	// from it; -1 for a new cell), until its fields are carried over.
	struct Rebuild {
		std::shared_ptr<BotWorkers::Job> job;
		std::shared_ptr<std::shared_ptr<TitanGrid>> grid;
		uint32_t version = 0;
		long applyAt = 0;
	} rebuild;
	std::vector<int> fromOld;

	// Where that thing's field is made about: where it is, once it has moved 3 m and its field is a second old (dropped then).
	const physics::Vec3& seedOf(uint32_t key, const physics::Vec3& at, float now)
	{
		auto& [seed, made] = seeds[key];
		if (!fields.count(key) || (now - made > 1 && physics::lengthSquared(at - seed) > 9)) {
			fields.erase(key);
			pending.erase(key);
			seed = at;
			made = now;
		}
		return seed;
	}

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
	TP("titanGrid");
	std::shared_ptr<TitanGrid>& held = m_titanGrids[{titan.object, team}];
	const uint32_t version = m_world.titanMeshVersion(titan.object);
	if (held && held->version == version) {
		return *held;
	}
	// A part going down or coming back changes the faces in a box only:
	// the rays that miss it are kept from the last grid, only those that
	// may meet it are cast again (a whole grid is over a million rays).
	physics::Vec3 low, high;
	bool changed = true;
	const bool known = held && m_world.titanMeshChange(titan.object, held->version, low, high, changed);
	if (known && !changed) {
		held->version = version;
		return *held;
	}
	const std::shared_ptr<const TitanGrid> old = known ? held : nullptr;
	if (held && m_settings.asyncPaths()) {
		// enhanced.botAsyncPaths: the new grid is made off the main thread from this one and the Titan's
		// collision as it stands now, and taken kTitanJobDelay ticks on (applyTitanJobs); the bots walk this
		// one meanwhile. A later change asks for another.
		if (held->rebuild.job && held->rebuild.version == version) {
			return *held;
		}
		if (const auto collision = m_world.titanCollision(titan.object)) {
			auto made = std::make_shared<std::shared_ptr<TitanGrid>>();
			const float radius = titan.radius;
			held->rebuild.grid = made;
			held->rebuild.version = version;
			held->rebuild.applyAt = m_tick + kTitanJobDelay;
			held->rebuild.job = workers().post([made, old, low, high, radius, version, collision, team] {
				*made = buildTitanGrid(old, low, high, radius, version,
					[&](const physics::Vec3& a, const physics::Vec3& b, float& f) { return collision->ray(team, a, b, f); });
			});
			return *held;
		}
	}
	held = buildTitanGrid(old, low, high, titan.radius, version,
		[&](const physics::Vec3& a, const physics::Vec3& b, float& f) { return m_world.titanLocalRay(titan.object, team, a, b, f); });
	return *held;
}

std::shared_ptr<BotManager::TitanGrid> BotManager::buildTitanGrid(const std::shared_ptr<const TitanGrid>& old, const physics::Vec3& low,
	const physics::Vec3& high, float radius, uint32_t version, const std::function<bool(const physics::Vec3&, const physics::Vec3&, float&)>& ray)
{
	auto made = std::make_shared<TitanGrid>();
	TitanGrid& g = *made;
	g.version = version;
	// shortcut: a fixed 160 m half-extent (the info's radius is shorter than the hull); upgrade to the mesh's box.
	const float r = std::max(radius, 160.0f);
	g.minX = g.minZ = -r;
	g.nx = g.nz = static_cast<int>(2 * r / kCell) + 1;
	g.columns.resize(static_cast<size_t>(g.nx) * g.nz);
	const bool reuse = old && old->nx == g.nx && old->nz == g.nz && old->minX == g.minX && old->minZ == g.minZ;
	constexpr float kSlack = 0.05f;
	// Whether a ray's box (two points' with the heights above them) misses the changed faces.
	const auto clear = [&](const physics::Vec3& a, const physics::Vec3& b, float above) {
		return std::max(a.x, b.x) < low.x - kSlack || std::min(a.x, b.x) > high.x + kSlack || std::max(a.z, b.z) < low.z - kSlack
			|| std::min(a.z, b.z) > high.z + kSlack || std::max(a.y, b.y) + above < low.y - kSlack || std::min(a.y, b.y) > high.y + kSlack;
	};
	std::vector<int> fromOld; // each cell's in the last grid, -1 for a new one
	// The floors: down each column, every surface with a soldier's headroom over it.
	for (int iz = 0; iz < g.nz; ++iz) {
		for (int ix = 0; ix < g.nx; ++ix) {
			const float x = g.minX + ix * kCell, z = g.minZ + iz * kCell;
			std::vector<int>& column = g.columns[static_cast<size_t>(iz) * g.nx + ix];
			if (reuse && clear({x, kBottom, z}, {x, kTop, z}, 0)) {
				for (int c : old->columns[static_cast<size_t>(iz) * g.nx + ix]) {
					column.push_back(static_cast<int>(g.cells.size()));
					g.cells.push_back({old->cells[static_cast<size_t>(c)].at, {}, {}});
					fromOld.push_back(c);
				}
				continue;
			}
			float y = kTop, f = 1;
			for (int n = 0; n < 24 && y > kBottom + 1 && ray({x, y, z}, {x, kBottom, z}, f); ++n) {
				const float floor = y + (kBottom - y) * f;
				float up = 1;
				if (!ray({x, floor + 0.35f, z}, {x, floor + kHeadroom, z}, up)) {
					column.push_back(static_cast<int>(g.cells.size()));
					g.cells.push_back({{x, floor, z}, {}, {}});
					fromOld.push_back(-1);
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
				const int oldA = reuse ? fromOld[static_cast<size_t>(a)] : -1;
				for (int dz = -1; dz <= 1; ++dz) {
					for (int dx = -1; dx <= 1; ++dx) {
						const std::vector<int>* col = dx || dz ? g.column(ix + dx, iz + dz) : nullptr;
						if (!col) {
							continue;
						}
						for (int b : *col) {
							const physics::Vec3 pb = g.cells[static_cast<size_t>(b)].at;
							const float dy = pb.y - pa.y, length = std::sqrt(physics::lengthSquared(pb - pa));
							const bool level = std::fabs(dy) <= kStep;
							if (!level && !(dy < 0 && dy >= -kDrop && (dx == 0 || dz == 0))) {
								continue;
							}
							const float cost = level ? length : length + 2;
							const int oldB = oldA >= 0 ? fromOld[static_cast<size_t>(b)] : -1;
							// The rays below lie 0.5..1.6 m over the two floors.
							if (oldB >= 0 && clear(pa, pb, 1.6f)) {
								const auto& was = old->cells[static_cast<size_t>(oldA)].out;
								if (std::any_of(was.begin(), was.end(), [&](const auto& link) { return link.first == oldB; })) {
									g.cells[static_cast<size_t>(a)].out.push_back({b, cost});
								}
								continue;
							}
							float f = 1;
							if (level) {
								if (!ray(pa + physics::Vec3 {0, 0.6f, 0}, pb + physics::Vec3 {0, 0.6f, 0}, f)
									&& !ray(pa + physics::Vec3 {0, 1.6f, 0}, pb + physics::Vec3 {0, 1.6f, 0}, f)) {
									g.cells[static_cast<size_t>(a)].out.push_back({b, cost});
								}
							} else {
								const physics::Vec3 over {pb.x, pa.y + 1.0f, pb.z};
								if (!ray(pa + physics::Vec3 {0, 1.0f, 0}, over, f) && !ray(over, pb + physics::Vec3 {0, 0.5f, 0}, f)) {
									g.cells[static_cast<size_t>(a)].out.push_back({b, cost});
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
	if (reuse) {
		g.fromOld = std::move(fromOld);
	}
	return made;
}

// The Titan a player on foot is aboard: the one whose grid is under him,
// else the nearest whose box he is in. The boxes (160 m about each) overlap
// once the commanders bring the Titans within 320 m of each other: the
// first one found would put a soldier on his own deck aboard the enemy's.
const GameWorld::TitanInfo* BotManager::titanUnder(const std::vector<GameWorld::TitanInfo>& titans, const GameWorld::PlayerInfo& player)
{
	const GameWorld::TitanInfo* nearest = nullptr;
	for (const GameWorld::TitanInfo& t : titans) {
		if (t.wreck || !aboard(t, player.position)) {
			continue;
		}
		if (titanGrid(t, player.team).cellAt(m_world.titanToLocal(t.object, player.position)) >= 0) {
			return &t;
		}
		if (!nearest || horizontal(t.position - player.position) < horizontal(nearest->position - player.position)) {
			nearest = &t;
		}
	}
	return nearest;
}

void BotManager::prepareTitanGrids(uint32_t titanObject)
{
	TP("prepareTitanGrids");
	if (m_settings.titanPlay() && m_world.gameMode() == "gpm_ti") {
		if (m_settings.asyncPaths()) {
			// enhanced.botAsyncPaths: the grids not made yet are made on the threads side by side (and here),
			// as titanGrid would make them one after another (the shared queries give the same answers).
			std::vector<std::pair<std::pair<uint32_t, int>, std::shared_ptr<BotWorkers::Job>>> jobs;
			std::vector<std::shared_ptr<std::shared_ptr<TitanGrid>>> made;
			for (const GameWorld::TitanInfo& titan : m_world.titans()) {
				const auto collision = titanObject && titan.object != titanObject ? nullptr : m_world.titanCollision(titan.object);
				for (int team : {1, 2}) {
					if (const auto held = m_titanGrids.find({titan.object, team}); !collision || (held != m_titanGrids.end() && held->second)) {
						continue;
					}
					const uint32_t version = m_world.titanMeshVersion(titan.object);
					const float radius = titan.radius;
					auto grid = std::make_shared<std::shared_ptr<TitanGrid>>();
					made.push_back(grid);
					jobs.push_back({{titan.object, team}, workers().post([grid, collision, team, version, radius] {
						*grid = buildTitanGrid(nullptr, {}, {}, radius, version,
							[&](const physics::Vec3& a, const physics::Vec3& b, float& f) { return collision->ray(team, a, b, f); });
					})});
				}
			}
			for (size_t i = 0; i < jobs.size(); ++i) {
				workers().wait(jobs[i].second);
				m_titanGrids[jobs[i].first] = *made[i];
			}
		}
		for (const GameWorld::TitanInfo& titan : m_world.titans()) {
			if (titanObject && titan.object != titanObject) {
				continue;
			}
			for (int team : {1, 2}) {
				titanGrid(titan, team);
			}
		}
		if (!m_settings.asyncPaths()) {
			return;
		}
		// enhanced.botAsyncPaths: the consoles' and the core's fields and the landing cells too, made on the
		// threads (and here) now rather than by the first bots to ask mid-round (the first tick, the first pod).
		for (const GameWorld::TitanInfo& titan : m_world.titans()) {
			if ((titanObject && titan.object != titanObject) || titan.wreck) {
				continue;
			}
			const auto collision = m_world.titanCollision(titan.object);
			for (int team : {1, 2}) {
				const std::shared_ptr<TitanGrid>& grid = m_titanGrids[{titan.object, team}];
				if (!grid) {
					continue;
				}
				for (const GameWorld::TitanPartInfo& part : m_world.titanParts(titan.team, titan.object)) {
					if ((part.kind == GameWorld::TitanPartInfo::Console || part.kind == GameWorld::TitanPartInfo::Core) && !part.wreck
						&& !grid->fields.count(part.id)) {
						grid->specs[part.id] = {m_world.titanToLocal(titan.object, part.position)};
						requestTitanField(grid, titan, team, part.id, collision, m_tick);
					}
				}
				grid->landingStale = !grid->landingDone;
			}
		}
		applyTitanJobs(true);
	}
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
		return found->second; // (enhanced.botAsyncPaths: perhaps the last grid's, until its new one is taken)
	}
	TP("titanField");
	g.specs[part] = {point, nearestOnly, entryRadius, around};
	const FieldQuery q = fieldQuery(m_world, g, titan, part, g.specs[part]);
	std::vector<float>& field = g.fields[part];
	makeField(
		g, q, [&](const physics::Vec3& a, const physics::Vec3& b) { return m_world.titanLocalShot(titan.object, a, b); },
		[&](const physics::Vec3& a, const physics::Vec3& b, float& f) { return m_world.titanLocalRay(titan.object, team, a, b, f); }, field);
	return field;
}

BotWorkers& BotManager::workers()
{
	if (!m_workers) {
		m_workers = std::make_unique<BotWorkers>(m_workerThreads >= 0 ? m_workerThreads : BotWorkers::defaultThreads());
	}
	return *m_workers;
}

void BotManager::setPathWorkers(int threads)
{
	m_workers.reset();
	m_workerThreads = threads;
}

// enhanced.botAsyncPaths: the field under `key` made anew off the main thread, on the grid as it is and the
// Titan's collision as it stands now (`collision`, shared by a grid's jobs), taken kTitanJobDelay ticks on.
void BotManager::requestTitanField(const std::shared_ptr<TitanGrid>& grid, const GameWorld::TitanInfo& titan, int team, uint32_t key,
	const std::shared_ptr<const GameWorld::TitanCollision>& collision, long applyAt)
{
	TitanGrid& g = *grid;
	const auto spec = g.specs.find(key);
	if (spec == g.specs.end() || !collision) {
		return;
	}
	auto query = std::make_shared<const FieldQuery>(fieldQuery(m_world, g, titan, key, spec->second));
	auto field = std::make_shared<std::vector<float>>();
	std::shared_ptr<const TitanGrid> cells = grid;
	TitanGrid::Pending& p = g.pending[key];
	p.field = field;
	p.applyAt = applyAt;
	p.job = workers().post([cells, query, collision, field, team] {
		makeField(
			*cells, *query, [&](const physics::Vec3& a, const physics::Vec3& b) { return collision->shot(a, b); },
			[&](const physics::Vec3& a, const physics::Vec3& b, float& f) { return collision->ray(team, a, b, f); }, *field);
	});
}

// The landing cells made anew off the main thread from the fields as they are now.
void BotManager::requestTitanLanding(const std::shared_ptr<TitanGrid>& grid, const GameWorld::TitanInfo& titan, int team,
	const std::shared_ptr<const GameWorld::TitanCollision>& collision, long applyAt)
{
	TitanGrid& g = *grid;
	if (!collision) {
		return;
	}
	auto best = std::make_shared<std::vector<float>>(g.cells.size(), kInfinity);
	for (const GameWorld::TitanPartInfo& part : m_world.titanParts(titan.team, titan.object)) {
		if ((part.kind == GameWorld::TitanPartInfo::Console || part.kind == GameWorld::TitanPartInfo::Core) && !part.wreck) {
			const std::vector<float>& f = titanField(g, titan, team, part.id, m_world.titanToLocal(titan.object, part.position));
			for (size_t c = 0; c < f.size(); ++c) {
				(*best)[c] = std::min((*best)[c], f[c]);
			}
		}
	}
	auto landing = std::make_shared<std::vector<int>>();
	std::shared_ptr<const TitanGrid> cells = grid;
	g.landingJob.landing = landing;
	g.landingJob.applyAt = applyAt;
	g.landingJob.job = workers().post([cells, best, collision, landing, team] {
		*landing = makeLanding(*cells, *best, [&](const physics::Vec3& a, const physics::Vec3& b, float& f) { return collision->ray(team, a, b, f); });
	});
}

// A new grid (a part went down or came back) starts with the last one's fields and landing cells, carried over
// to its cells, and has them made anew off the main thread (enhanced.botAsyncPaths). `fromOld`: each cell's in
// the last grid (-1 for a new one; empty: found by place).
void BotManager::carryTitanFields(const TitanGrid& old, const std::shared_ptr<TitanGrid>& grid, const std::vector<int>& fromOld,
	const GameWorld::TitanInfo& titan, int team)
{
	TitanGrid& g = *grid;
	std::vector<int> map(g.cells.size(), -1);
	std::vector<int> toNew(old.cells.size(), -1);
	for (size_t c = 0; c < g.cells.size(); ++c) {
		map[c] = fromOld.size() == g.cells.size() ? fromOld[c] : old.cellAt(g.cells[c].at);
		if (map[c] >= 0 && static_cast<size_t>(map[c]) < old.cells.size()) {
			toNew[static_cast<size_t>(map[c])] = static_cast<int>(c);
		} else {
			map[c] = -1;
		}
	}
	// Not the wrecked parts' (no one walks to them now).
	std::set<uint32_t> wrecked;
	for (const GameWorld::TitanPartInfo& part : m_world.titanParts(titan.team, titan.object)) {
		if (part.wreck) {
			wrecked.insert(part.id);
		}
	}
	const auto collision = m_world.titanCollision(titan.object);
	for (const auto& [key, was] : old.fields) {
		if (wrecked.count(key) || !old.specs.count(key)) {
			continue;
		}
		std::vector<float>& f = g.fields[key];
		f.assign(g.cells.size(), kInfinity);
		for (size_t c = 0; c < g.cells.size(); ++c) {
			if (map[c] >= 0 && static_cast<size_t>(map[c]) < was.size()) {
				f[c] = was[static_cast<size_t>(map[c])];
			}
		}
		g.specs[key] = old.specs.at(key);
		requestTitanField(grid, titan, team, key, collision, m_tick + kTitanJobDelay);
	}
	g.seeds = old.seeds;
	if (old.landingDone) {
		g.landingDone = true;
		for (int c : old.landing) {
			if (toNew[static_cast<size_t>(c)] >= 0) {
				g.landing.push_back(toNew[static_cast<size_t>(c)]);
			}
		}
		g.landingStale = true;
	}
}

// The jobs due by this tick (all of them with `all`) taken, waiting for any not done yet; a grid whose fields
// are all new again has its landing cells made anew.
void BotManager::applyTitanJobs(bool all)
{
	TP("applyTitanJobs");
	for (bool more = true; more;) {
		more = false;
		for (auto& [key, grid] : m_titanGrids) {
			if (!grid) {
				continue;
			}
			if (grid->rebuild.job && (all || grid->rebuild.applyAt <= m_tick)) {
				workers().wait(grid->rebuild.job);
				const std::shared_ptr<TitanGrid> made = *grid->rebuild.grid;
				grid->rebuild = {};
				for (const GameWorld::TitanInfo& titan : m_world.titans()) {
					if (titan.object == key.first && made) {
						carryTitanFields(*grid, made, made->fromOld, titan, key.second);
						made->fromOld.clear();
						grid = made;
						more = all;
					}
				}
			}
			TitanGrid& g = *grid;
			for (auto it = g.pending.begin(); it != g.pending.end();) {
				if (!all && it->second.applyAt > m_tick) {
					++it;
					continue;
				}
				workers().wait(it->second.job);
				g.fields[it->first] = std::move(*it->second.field);
				it = g.pending.erase(it);
			}
			if (g.landingJob.job && (all || g.landingJob.applyAt <= m_tick)) {
				workers().wait(g.landingJob.job);
				g.landing = std::move(*g.landingJob.landing);
				g.landingDone = true;
				g.landingJob = {};
			}
			if (g.landingStale && g.pending.empty() && !g.landingJob.job) {
				g.landingStale = false;
				for (const GameWorld::TitanInfo& titan : m_world.titans()) {
					if (titan.object == key.first && !titan.wreck) {
						requestTitanLanding(grid, titan, key.second, m_world.titanCollision(titan.object), m_tick + kTitanJobDelay);
						more = all;
					}
				}
			}
		}
	}
}

bool BotManager::titanGunWalk(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds,
	const physics::Vec3& entry, float radius, uint32_t key, const char* behaviour)
{
	TP("titanGunWalk");
	if (!m_settings.titanPlay()) {
		return false;
	}
	const auto titans = m_world.titans();
	const auto* titan = titanOf(titans, self.team);
	if (!titan) {
		return false;
	}
	TitanGrid& grid = titanGrid(*titan, self.team);
	// Entry fields have their own keys (a gun seat's, a pod array's), separate from object/boarder fields.
	const auto& field = titanField(grid, *titan, self.team, key, m_world.titanToLocal(titan->object, entry), true, radius);
	bot.behaviour = behaviour;
	return titanWalk(bot, self, input, seconds, grid, *titan, field);
}

bool BotManager::titanPlay(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	TP("titanPlay");
	const float cutOffSince = bot.titanCutOffSince; // held only while each think finds it so
	bot.titanCutOffSince = -1;
	if (!m_settings.titanPlay() || self.vehicle || m_world.gameMode() != "gpm_ti") {
		return false;
	}
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	const GameWorld::TitanInfo* titan = titanUnder(titans, self);
	if (!titan) {
		return false;
	}
	const bool own = titan->team == self.team;
	// Cut off aboard its own Titan: off the grid, or where no whole console
	// (nor the core) can be walked to: a ledge of the outer hull it dropped
	// onto (a drop is one way), a spot a pod set it down. The guns', the
	// crews' and the pods' ways all start on the grid and the ordinary AI
	// has no navmesh here, so it would stand there for good. After 4 s so
	// (a stair or a pod's landing is off the grid for a moment) it leaves
	// as a boarder cut off does.
	if (own) {
		TitanGrid& g = titanGrid(*titan, self.team);
		const int cell = g.cellAt(m_world.titanToLocal(titan->object, self.position));
		bool cut = true;
		for (const GameWorld::TitanPartInfo& part : cell < 0 ? std::vector<GameWorld::TitanPartInfo> {} : m_world.titanParts(titan->team, titan->object)) {
			if ((part.kind == GameWorld::TitanPartInfo::Console || part.kind == GameWorld::TitanPartInfo::Core) && !part.wreck
				&& titanField(g, *titan, self.team, part.id, m_world.titanToLocal(titan->object, part.position))[static_cast<size_t>(cell)] < kInfinity) {
				cut = false;
				break;
			}
		}
		if (cut) {
			bot.titanCutOffSince = cutOffSince < 0 ? m_time : cutOffSince;
			if (m_time - bot.titanCutOffSince > 4) {
				return titanLeave(bot, self, input, seconds, *titan);
			}
		}
	}
	// The enemies aboard this Titan.
	std::vector<const GameWorld::PlayerInfo*> boarders;
	if (own) {
		for (const GameWorld::PlayerInfo& p : m_players) {
			if (p.alive && p.team != self.team && !p.vehicle && titanUnder(titans, p) == titan) {
				boarders.push_back(&p);
			}
		}
		if (boarders.empty()) {
			// No one to fight: a boarder walks to the transport on the deck and
			// gets in; a defender stands by a console; the rest take the guns
			// and the pods, as before.
			const int role = titanRole(self);
			if (role != 1 && titanRepair(bot, self, input, seconds, *titan)) {
				return true;
			}
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
					titanField(g, *titan, self.team, transport->object,
						g.seedOf(transport->object, m_world.titanToLocal(titan->object, transport->position), m_time), true));
				return true;
			}
			std::vector<const GameWorld::TitanPartInfo*> posts;
			for (const GameWorld::TitanPartInfo& part : m_world.titanParts(titan->team, titan->object)) {
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
	// Aboard the enemy's Titan, 6 s of that and then 4 s of the way on: a
	// defender out of reach would otherwise hold a boarder there for good.
	if (fireUrgency(bot, self, m_players) > 0 && bot.target >= 0 && (own || m_time >= bot.titanFireRestUntil)) {
		const auto known = std::find_if(bot.known.begin(), bot.known.end(), [&](const Known& k) { return k.player == bot.target; });
		if (known != bot.known.end() && known->lostAt < 0) {
			if (bot.titanFireFrom < 0) {
				bot.titanFireFrom = m_time;
			} else if (!own && m_time - bot.titanFireFrom > 6) {
				bot.titanFireFrom = -1;
				bot.titanFireRestUntil = m_time + 4;
			}
			bot.behaviour = "Fire";
			fire(bot, self, input, seconds);
			input.throttle = input.strafe = 0;
			handleWeapon(bot, self, input);
			return true;
		}
	}
	bot.titanFireFrom = -1;
	TitanGrid& g = titanGrid(*titan, self.team);
	const physics::Vec3 local = m_world.titanToLocal(titan->object, self.position);
	const int cell = g.cellAt(local);
	if (cell < 0) {
		return !own && titanLeave(bot, self, input, seconds, *titan);
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
		const std::vector<float>& f = titanField(g, *titan, self.team, key, g.seedOf(key, at, m_time));
		if (f[static_cast<size_t>(cell)] < kInfinity) {
			bot.behaviour = "TitanHunt";
			if (f[static_cast<size_t>(cell)] <= 0) {
				aim(bot, self, prey->position + physics::Vec3 {0, 1.2f, 0}, {}, true, {}, input, seconds); // facing him
			}
			return titanWalk(bot, self, input, seconds, g, *titan, f);
		}
	}
	// The objective: the cheapest whole console to reach; none left, the core.
	const std::vector<GameWorld::TitanPartInfo> parts = m_world.titanParts(titan->team, titan->object);
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
		return !own && titanLeave(bot, self, input, seconds, *titan);
	}
	bot.behaviour = own ? "TitanDefend" : "TitanAttack";
	// A boarder's gun: the one in hand while it has rounds (magazine or reserve), else the carried gun with
	// rounds left that hurts armour most (the ordinary weapon handling would take the dry primary back); none
	// left, off the Titan (a new life brings rounds) rather than standing there for good.
	bool loaded = false, primaryDry = false;
	int spare = 0;
	if (!own) {
		float spareStrength = -1;
		for (const GameWorld::WeaponAmmo& w : m_world.weaponAmmo(self.id)) {
			const WeaponAi ai = weaponAi(w.weapon);
			const bool gun = ai.type == WeaponAi::FireArm || ai.type == WeaponAi::Sniper || ai.type == WeaponAi::FireAndForget;
			for (const auto& [slot, name] : self.carried) {
				if (name != w.weapon) {
					continue;
				}
				primaryDry = primaryDry || (slot == 3 && w.rounds <= 0);
				if (gun && w.rounds > 0 && ai.strength[2] > spareStrength) {
					spare = slot;
					spareStrength = ai.strength[2];
				}
			}
			loaded = loaded || (gun && w.inHand && w.rounds > 0);
		}
		if (!loaded && !spare) {
			return titanLeave(bot, self, input, seconds, *titan);
		}
	}
	const auto gunInHand = [&] {
		if (!own && !loaded) {
			input.selectItem = spare;
		} else if (!own && primaryDry && input.selectItem == 3) {
			input.selectItem = 0;
		}
	};
	const float here = (*field)[static_cast<size_t>(cell)];
	if (here <= 0) {
		// At a spot that sees it: a defender waits; an attacker shoots in
		// bursts (a tap for a single-shot weapon, a burst for an automatic).
		input.throttle = input.strafe = 0;
		if (!own) {
			physics::Vec3 at = goal->position, local;
			if (m_world.titanPartAimPoint(goal->id, local)) {
				at = m_world.titanToWorld(titan->object, local); // the middle of its faces, which the spot's rounds reach
			}
			aim(bot, self, at, {}, true, {}, input, seconds);
			const physics::Vec3 eye = self.position + physics::Vec3 {0, 1.6f, 0};
			const physics::Vec3 d = at - eye;
			const float yawOff = std::remainder(std::atan2(d.x, d.z) * 57.29578f - (self.yaw + input.turn), 360.0f);
			// A teammate in the line of fire (two boarders on one spot): no shot, a step aside.
			bool mate = false;
			for (const GameWorld::PlayerInfo& p : m_players) {
				const physics::Vec3 chest = p.position + physics::Vec3 {0, 1.2f, 0};
				const float along = std::clamp(physics::dot(chest - eye, d) / std::max(physics::lengthSquared(d), 1e-4f), 0.0f, 1.0f);
				mate = mate || (p.id != self.id && p.alive && p.team == self.team && !p.vehicle && physics::lengthSquared(eye + d * along - chest) < 0.7f * 0.7f);
			}
			input.strafe = mate ? (self.id % 2 ? 1.0f : -1.0f) : 0.0f;
			input.fire = loaded && !mate && std::fabs(yawOff) < 4 && std::fmod(m_time, 0.8f) < 0.6f && self.roundsInMag != 0 && !self.reloading;
			input.reload = loaded && self.roundsInMag == 0 && !bot.reloadedLast;
		}
		handleWeapon(bot, self, input);
		gunInHand();
		return true;
	}
	const bool walked = titanWalk(bot, self, input, seconds, g, *titan, *field);
	gunInHand();
	return walked || (!own && titanLeave(bot, self, input, seconds, *titan));
}

// An engineer aboard its own Titan with no enemy aboard mends its parts with
// the repair tool, as a player may (retail's RepairVehicle, 0x10073360,
// looks at vehicles only): a damaged console, and a gun hub, its wreck too
// (canBeRepairedWhenWreck 1: back in service above 0 hit points,
// 0x00654120). A console's wreck can't be mended: the heal 0x00655a00
// refuses a wreck without canBeRepairedWhenWreck, which no console sets.
// Two engineers at most to a part, each standing with its eyes within
// abilityRadius + 1.5 m of one of the part's meshes: a gun hangs under the
// hull, out of reach, but its control station inside (an unarmoured child
// of the gun's bundle, eu_controlstation_v2_g / _aa) is the gun's to the tool.
bool BotManager::titanRepair(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds, const GameWorld::TitanInfo& titan)
{
	// (Any claim of the last think and the tool's AI target were let go at the top of this one, in BotManager::update.)
	const int slot = kitSlot(self, WeaponAi::Repair);
	const world::ObjectTemplate* tool = slot ? m_world.templates().find(weaponName(self, slot)) : nullptr;
	float reach = 0;
	bool ammo = false, whileFiring = false;
	if (!tool || !GameWorld::gadgetScan(*tool, reach, ammo, whileFiring) || ammo) {
		return false;
	}
	TitanGrid& g = titanGrid(titan, self.team);
	const int cell = g.cellAt(m_world.titanToLocal(titan.object, self.position));
	if (cell < 0) {
		return false;
	}
	// The tool's material against the part's (applyAbility's cell): the shipped tables give the repair tool (84)
	// nothing on the consoles' metal (4110) and all on the gun hubs' (4198), in both mods.
	const auto material = [](const world::ObjectTemplate* t, const char* key) -> uint32_t {
		if (!t) {
			return 0;
		}
		const auto it = t->settings().find(key);
		return it == t->settings().end() ? 0 : static_cast<uint32_t>(std::atoi(it->second.c_str()));
	};
	const uint32_t toolMaterial = material(tool, "ammo.abilityMaterial");
	// The parts the other engineers hold: two at most to a part.
	std::map<uint32_t, int> claimed;
	for (const auto& [id, other] : m_bots) {
		if (id != self.id && other.titanRepair) {
			++claimed[other.titanRepair];
		}
	}
	const std::vector<GameWorld::TitanPartInfo> parts = m_world.titanParts(titan.team, titan.object);
	const GameWorld::TitanPartInfo* goal = nullptr;
	const std::vector<float>* field = nullptr;
	float best = kInfinity;
	physics::Vec3 aimAt;
	for (const GameWorld::TitanPartInfo& part : parts) {
		if ((part.kind != GameWorld::TitanPartInfo::Gun && part.kind != GameWorld::TitanPartInfo::Console)
			|| part.hitPoints >= part.maxHitPoints || !m_world.repairable(part.id)) {
			continue;
		}
		if (const auto it = claimed.find(part.id); it != claimed.end() && it->second >= 2) {
			continue;
		}
		// Its armour's material and its meshes, once a grid: neither the template nor the meshes in the Titan's
		// frame change (a part going down makes a new grid).
		auto known = g.repairParts.find(part.id);
		if (known == g.repairParts.end()) {
			std::vector<physics::Vec3> meshes = m_world.titanPartMeshes(part.id);
			for (physics::Vec3& mesh : meshes) {
				mesh = m_world.titanToLocal(titan.object, mesh);
			}
			const uint32_t armour = material(m_world.templates().find(part.name), "armor.defaultMaterial");
			known = g.repairParts.emplace(part.id, std::make_pair(armour, std::move(meshes))).first;
		}
		const auto& [armour, meshes] = known->second;
		if (m_world.materials() && m_world.materials()->damageMod(toolMaterial, armour) <= 0) {
			continue;
		}
		// To any of its meshes (a gun's: its control station inside the hull; the gun itself hangs under it), each
		// field under its own key (a part's id less 0x10000000, the mesh's number at bit 20): the part's own field
		// is the shooters' (cells that see it).
		for (size_t k = 0; k < meshes.size() && k < 16; ++k) {
			const std::vector<float>& f = titanField(g, titan, self.team, (part.id - 0x10000000u) ^ static_cast<uint32_t>(k << 20),
				meshes[k] - physics::Vec3 {0, 0.5f, 0}, true, reach + 1.5f);
			if (f[static_cast<size_t>(cell)] < best) {
				best = f[static_cast<size_t>(cell)];
				goal = &part;
				field = &f;
				aimAt = m_world.titanToWorld(titan.object, meshes[k]);
			}
		}
	}
	if (!goal) {
		return false;
	}
	bot.titanRepair = goal->id;
	bot.behaviour = "TitanRepair";
	if (best > 0) {
		return titanWalk(bot, self, input, seconds, g, titan, *field);
	}
	// There: the tool on it. Its ray meets the part's mesh, or something else first (the Titan's own
	// forcefield over the decks), when the tool takes the AI's target instead (0x005d84a0, setAiAbilityTarget).
	m_world.setAiAbilityTarget(self.id, goal->id);
	input.selectItem = self.inHandSlot != slot ? slot : 0;
	aim(bot, self, aimAt, {}, true, {}, input, seconds);
	input.fire = self.inHandSlot == slot;
	input.throttle = input.strafe = 0;
	return true;
}

// A boarder with no way to any console or the core (a ledge of the hull it
// dropped onto and cannot climb back from, a spot off the grid): no navmesh
// covers a Titan, so the ordinary AI would stand there for good. Instead it
// walks out from the Titan's middle, jumping, until it drops off the hull;
// the parachute takes it down, and the ordinary AI on the ground.
bool BotManager::titanLeave(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds, const GameWorld::TitanInfo& titan)
{
	const physics::Vec3 local = m_world.titanToLocal(titan.object, self.position);
	physics::Vec3 out {local.x, 0, local.z};
	if (physics::lengthSquared(out) < 1) {
		out = {1, 0, 0};
	}
	// Wedged for 2 s (in the Titan's frame: it moves under the commander): a quarter turn, alternating sides each 4 s.
	if (physics::lengthSquared(local - bot.titanAt) > 1.0f) {
		bot.titanAt = local;
		bot.titanAtTime = m_time;
	} else if (m_time - bot.titanAtTime > 2) {
		const float side = std::fmod(m_time, 8.0f) < 4 ? 1.0f : -1.0f;
		out = {out.x - side * out.z, 0, out.z + side * out.x};
	}
	const physics::Vec3 d = m_world.titanToWorld(titan.object, local + out) - self.position;
	const float off = std::remainder(std::atan2(d.x, d.z) * 57.29578f - self.yaw, 360.0f);
	input.turn = std::clamp(off, -360 * seconds, 360 * seconds);
	input.throttle = std::fabs(off) < 60 ? 1.0f : 0.0f;
	input.strafe = 0;
	input.jump = std::fmod(m_time, 1.5f) < 0.2f;
	bot.behaviour = "TitanLeave";
	handleWeapon(bot, self, input);
	return true;
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
	if (physics::lengthSquared(local - bot.titanAt) > 1.0f) {
		bot.titanAt = local;
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
	TP("titanLandingSpots");
	std::vector<physics::Vec3> spots;
	for (const GameWorld::TitanInfo& titan : m_world.titans()) {
		if (titan.team != titanTeam || titan.wreck) {
			continue;
		}
		TitanGrid& g = titanGrid(titan, team);
		if (!g.landingDone) {
			g.landingDone = true;
			std::vector<float> best(g.cells.size(), kInfinity);
			for (const GameWorld::TitanPartInfo& part : m_world.titanParts(titan.team, titan.object)) {
				if ((part.kind == GameWorld::TitanPartInfo::Console || part.kind == GameWorld::TitanPartInfo::Core) && !part.wreck) {
					const std::vector<float>& f = titanField(g, titan, team, part.id, m_world.titanToLocal(titan.object, part.position));
					for (size_t c = 0; c < f.size(); ++c) {
						best[c] = std::min(best[c], f[c]);
					}
				}
			}
			g.landing = makeLanding(g, best, [&](const physics::Vec3& a, const physics::Vec3& b, float& f) {
				return m_world.titanLocalRay(titan.object, team, a, b, f);
			});
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
		return false;
	}
	bot.titanPodGoal = goal;
	bot.titanPodGoalFor = spots.front();
	bot.titanPodGoalSet = true;
	return true;
}

// A pod's thrusters against the way still to go to `spot`, less the way it drifts.
void BotManager::steerPod(const GameWorld::PlayerInfo& self, const physics::Vec3& spot, SoldierInput& input) const
{
	const world::Object* pod = m_world.objects().find(self.vehicle);
	if (!pod) {
		return;
	}
	const physics::Vec3 want = spot - self.position - self.velocity * 1.5f;
	const float yaw = pod->rotation().x * 0.017453292f;
	input.throttle = std::clamp((want.x * std::sin(yaw) + want.z * std::cos(yaw)) / 8, -1.0f, 1.0f);
	input.strafe = std::clamp((want.x * std::cos(yaw) - want.z * std::sin(yaw)) / 8, -1.0f, 1.0f);
}

// enhanced.botTitan: a drop from its own Titan (Remaster's CoOp maps park it as
// a spawn, Wake Island 2142's off the coast; in Titan mode it flies over
// Molokai's buildings, whose roofs have no mesh) aims for dry, walkable ground
// within a pod's reach, the place nearest its objective (the nearest control
// point the team doesn't hold: a silo in Titan mode), not wherever the array
// points. Boarding pods for the enemy Titan are titanPod's.
bool BotManager::podLanding(Bot& bot, const GameWorld::PlayerInfo& self, physics::Vec3& spot, const physics::Vec3* launcher)
{
	if (!m_settings.titanPlay()) {
		return false;
	}
	if (m_time - bot.podLandingAt < 20) { // chosen on the way to the array; kept for the aim and the ride
		spot = bot.podLanding;
		return true;
	}
	// From the launcher (a seated soldier's position is his Titan's, tens of metres off).
	const physics::Vec3 origin = launcher ? *launcher : self.position;
	const GameWorld::ControlPointInfo* goal = nullptr;
	const std::vector<GameWorld::ControlPointInfo> points = m_world.controlPointList();
	float best = kInfinity;
	for (const GameWorld::ControlPointInfo& p : points) {
		const float d = horizontal(p.position - origin) + (p.team == self.team || !p.capturable ? 1e5f : 0.0f);
		if (d < best) {
			best = d;
			goal = &p;
		}
	}
	if (!goal) {
		return false;
	}
	float water = -kInfinity;
	m_world.waterLevel(water);
	// A pod left to fall carries about 77 m from Wake's Titans; steered, further. Past
	// that reach: the nearest dry place, whatever the objective, as close as it gets.
	constexpr float kReach = 110;
	// Dry ground on the infantry mesh (within 2 m of it: not a slope or ledge beside it), there and
	// 10 m all round: a pod comes down within a few metres of where it was steered.
	const auto standable = [&](float x, float z) {
		const float y = m_world.terrainHeight(x, z);
		return y >= water + 1 && (!m_navMesh || m_navMesh->findPolygon(ai::Vec3 {x, y, z}, 2.0f) >= 0);
	};
	best = kInfinity;
	for (float r = 30; r <= 250; r += 10) {
		for (int k = 0; k < 72; ++k) {
			physics::Vec3 p = origin + physics::Vec3 {std::sin(k * 0.08726646f), 0, std::cos(k * 0.08726646f)} * r;
			p.y = m_world.terrainHeight(p.x, p.z);
			if (!standable(p.x, p.z) || !standable(p.x + 10, p.z) || !standable(p.x - 10, p.z) || !standable(p.x, p.z + 10)
				|| !standable(p.x, p.z - 10)) {
				continue;
			}
			const float d = r <= kReach ? horizontal(p - goal->position) : 1e5f + r;
			if (d < best) {
				best = d;
				spot = p;
			}
		}
	}
	if (best >= kInfinity) {
		return false;
	}
	bot.podLanding = spot;
	bot.podLandingAt = m_time;
	return true;
}

bool BotManager::titanPod(Bot& bot, const GameWorld::PlayerInfo& self, SoldierInput& input, float seconds)
{
	if (bot.titanSortieVehicle && bot.titanSortieVehicle != self.vehicle) {
		bot.titanSortieVehicle = 0;
		bot.titanSortie = 0;
		bot.titanSupportSilo = -1;
		bot.titanSupportStep = 0;
		bot.titanSupportUntil = 0;
	}
	if (bot.titanSortieRider != self.vehicle) {
		bot.titanSortieRider = 0;
	}
	if (!self.vehicle) {
		bot.titanSortie = 0;
	}
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
		steerPod(self, *spot, input);
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
		if (bot.titanSortieVehicle == self.vehicle) {
			bot.titanSortie = 2;
		}
		bot.titanSortieRider = 0;
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
		return fly(bot, self, input, seconds);
	}
	if (titanSortie(bot, self, input, seconds)) {
		return true;
	}
	if (enemy && enemy->shield && podSeat(seats[0]) && object && object->objectTemplate()
		&& (m_settings.typeFlags(aiTemplateOf(object->objectTemplate()->name())) & kITAir)) {
		return false;
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
		if (me.titanCrew == v.object && rank < 3) {
			return &v; // a rider's claim gives way to a gunship gunner's seat
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
	const uint32_t key = 0x20000000u ^ v->object;
	titanWalk(bot, self, input, seconds, g, *home,
		titanField(g, *home, self.team, key, g.seedOf(key, m_world.titanToLocal(home->object, entry), m_time), true, 0, outside ? v->object : 0));
	return true;
}

// A transport of its own Titan's deck while the enemy's shield is up (with
// it down, titanPod's boarding run): its pilot waits 6..18 s on the deck for
// riders (20 s more while one is still walking), then flies to a silo the
// team doesn't hold. Riders leave by pod over the silo, or if the pilot is
// lost. Gunners stay aboard and use ordinary combat AI; after the drop the
// pilot supports a silo for 20 s, then repositions to another.
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
	const auto gunnerSeat = [&](size_t seat) {
		if (seat >= seats.size()) {
			return false;
		}
		const auto armed = [&](const GameWorld::SeatWeapon& w) {
			if (w.weapon.empty() || w.podLauncher) {
				return false;
			}
			const WeaponAi ai = weaponAi(w.weapon);
			return std::any_of(ai.strength.begin(), ai.strength.end(), [](float strength) { return strength > 0; });
		};
		return armed(seats[seat]) || std::any_of(seats[seat].weapons.begin(), seats[seat].weapons.end(), armed);
	};
	// Same-team deck aircraft already flying to (or holding over) a silo: each
	// counts 300 m against it, so the team's aircraft spread over the silos.
	const auto siloTaken = [&](int point) {
		int taken = 0;
		for (const auto& [id, other] : m_bots) {
			const GameWorld::PlayerInfo* mate = id == self.id ? nullptr : playerOf(id);
			taken += mate && mate->alive && mate->driving && mate->team == self.team
				&& ((other.titanSortie == 1 && other.titanSortieSilo == point) || (other.titanSortie == 2 && other.titanSupportSilo == point));
		}
		return taken;
	};
	if (!podSeat(seats[0])) {
		// A gunship from the deck: off it and 200 m clear of the Titan (high,
		// toward the nearest silo the team doesn't hold, else ahead of the
		// Titan), then the ordinary pilot's. On the deck that one waits
		// while soldiers are near, and the Titan's crew always is.
		// (Not while enhanced.botPilots has it back on the deck to be mended: evade's retreat.)
		if (bot.retreating && m_settings.pilotsHunt()) {
			bot.titanSortie = 0;
			return false;
		}
		bot.titanSortie = self.driving && horizontal(self.position - home->position) < 200
			&& (bot.titanSortie == 1 || aboard(*home, self.position)) ? 1 : 0;
		if (!bot.titanSortie) {
			return false;
		}
		// On the deck it waits up to 20 s for a gunner walking to it (3 s for one to set off).
		if (aboard(*home, self.position) && !bot.heli.airborne) {
			if (bot.titanWaitFor != self.vehicle) {
				bot.titanWaitFor = self.vehicle;
				bot.titanWaitUntil = m_time + 20;
			}
			bool coming = m_time < bot.titanWaitUntil - 17;
			for (const auto& [id, other] : m_bots) {
				const GameWorld::PlayerInfo* mate = id == self.id || other.titanCrew != self.vehicle ? nullptr : playerOf(id);
				coming = coming || (mate && mate->team == self.team && mate->alive && !mate->vehicle);
			}
			if (coming && m_time < bot.titanWaitUntil && occupants.size() > 1 && occupants[1] < 0) {
				bot.behaviour = "TitanTakeoff";
				bot.titanFlyMode = 1;
				return fly(bot, self, input, seconds);
			}
		}
		const GameWorld::SiloInfo* best = nullptr;
		float bestScore = kInfinity;
		for (const GameWorld::SiloInfo& silo : silos) {
			const float score = horizontal(silo.position - self.position) + 300.0f * siloTaken(silo.controlPoint);
			if (silo.team != self.team && score < bestScore) {
				best = &silo;
				bestScore = score;
			}
		}
		bot.titanSortieSilo = best ? best->controlPoint : -1;
		physics::Vec3 to = best ? best->position : m_world.titanToWorld(home->object, {0, 0, 400});
		// enhanced.botPilots: at least 300 m out from the Titan (a silo it stands over would hold the gunship
		// hovering there for good, never 200 m clear), and the sortie over after 25 s.
		if (m_settings.pilotsHunt()) {
			physics::Vec3 out = to - home->position;
			out.y = 0;
			const float l = horizontal(out);
			out = l > 1 ? out * (1 / l) : m_world.titanToWorld(home->object, {0, 0, 1}) - home->position;
			if (l < 300) {
				to = home->position + out * 300.0f;
			}
			if (bot.titanSortieFor != self.vehicle || !bot.heli.airborne) { // (from its take-off)
				bot.titanSortieFor = self.vehicle;
				bot.titanSortieAt = m_time;
			}
			if (bot.heli.airborne && m_time - bot.titanSortieAt > 25) {
				bot.titanSortie = 0;
				return false;
			}
		}
		bot.behaviour = "TitanTakeoff";
		bot.titanFlyTo = {to.x, std::max(m_world.terrainHeight(to.x, to.z) + 70, m_world.titanToWorld(home->object, {0, 40, 0}).y), to.z};
		bot.titanFlyMode = 4;
		return fly(bot, self, input, seconds);
	}
	const bool onDeck = aboard(*home, self.position);
	const auto overDeck = [&] {
		if (!onDeck) {
			return false;
		}
		const physics::Vec3 local = m_world.titanToLocal(home->object, self.position);
		float fraction = 1;
		return m_world.titanLocalRay(home->object, self.team, local + physics::Vec3 {0, 1, 0},
			local - physics::Vec3 {0, 8, 0}, fraction);
	};
	if (!self.driving) {
		bot.titanSortieVehicle = 0;
		const int pilotId = !occupants.empty() ? occupants[0] : -1;
		const bool pilot = pilotId >= 0;
		if (pilot) {
			const auto found = m_bots.find(pilotId);
			const bool active = found != m_bots.end()
				&& ((found->second.titanSortieVehicle == self.vehicle && found->second.titanSortie < 2)
					|| (found->second.titanSortieVehicle != self.vehicle && overDeck()));
			bot.titanSortieRider = active ? self.vehicle : 0;
		}
		if (bot.titanSortieRider != self.vehicle) {
			return false;
		}
		if (onDeck && pilot) {
			bot.behaviour = "TitanSortie";
			return true; // seated until it is off
		}
		if (pilot && gunnerSeat(static_cast<size_t>(self.seat))) {
			return false;
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
	bot.titanSortieRider = 0;
	int riders = 0;
	for (size_t seat = 1; seat < occupants.size(); ++seat) {
		riders += occupants[seat] >= 0 && !gunnerSeat(seat);
	}
	if (bot.titanSortieVehicle != self.vehicle) {
		bot.titanSortieVehicle = self.vehicle;
		bot.titanSortie = overDeck() ? 0 : 3;
		if (bot.titanSortie == 3) {
			return false;
		}
		bot.titanWaitFor = self.vehicle;
		bot.titanWaitUntil = m_time + 6 + 12 * random();
	}
	if (bot.titanSortie == 3) {
		return false;
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
		float bestScore = kInfinity;
		for (const GameWorld::SiloInfo& silo : silos) {
			const float score = horizontal(silo.position - self.position) + 300.0f * siloTaken(silo.controlPoint);
			if (silo.team != self.team && score < bestScore) {
				best = &silo;
				bestScore = score;
			}
		}
		return best;
	};
	if (bot.titanSortie == 0) {
		bool coming = false;
		for (const auto& [id, other] : m_bots) {
			const GameWorld::PlayerInfo* mate = id == self.id || other.titanCrew != self.vehicle ? nullptr : playerOf(id);
			coming = coming || (mate && mate->team == self.team && mate->alive && !mate->vehicle);
		}
		const int free = static_cast<int>(std::count(occupants.begin(), occupants.end(), -1)) + static_cast<int>(seats.size() - std::min(seats.size(), occupants.size()));
		const bool go = free == 0 || m_time >= bot.titanWaitUntil + (coming ? 20 : 0);
		const GameWorld::SiloInfo* silo = go ? nearestSilo() : nullptr;
		if (!silo) {
			if (onDeck && !go) {
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
		if (!silo && (silo = nearestSilo())) {
			bot.titanSortieSilo = silo->controlPoint;
			bot.titanSortieUntil = 0;
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
		const GameWorld::SiloInfo* support = nullptr;
		for (const auto& silo : silos) {
			if (silo.controlPoint == bot.titanSupportSilo) {
				support = &silo;
			}
		}
		const bool next = bot.titanSupportUntil > 0 && m_time >= bot.titanSupportUntil;
		if (!support || next) {
			const int previous = bot.titanSupportSilo;
			float best = kInfinity;
			for (const auto& silo : silos) {
				if (silos.size() > 1 && silo.controlPoint == previous) {
					continue;
				}
				int assigned = 0;
				for (const auto& [id, other] : m_bots) {
					const auto* mate = id == self.id ? nullptr : playerOf(id);
					assigned += mate && mate->alive && mate->driving && mate->team == self.team
						&& other.titanSupportSilo == silo.controlPoint;
				}
				const float score = horizontal(silo.position - self.position)
					+ 300.0f * assigned + (silo.team == self.team ? 200.0f : 0.0f);
				if (score < best) {
					best = score;
					support = &silo;
				}
			}
			if (!support) {
				return false;
			}
			bot.titanSupportSilo = support->controlPoint;
			bot.titanSupportUntil = 0;
			++bot.titanSupportStep;
		}
		// Within the side guns' 220 m reach, with separate support stations.
		const float angle = (self.id % 8 + bot.titanSupportStep) * 0.78539816f;
		physics::Vec3 station = support->position + physics::Vec3 {150 * std::sin(angle), 0, 150 * std::cos(angle)};
		const float clear = horizontal(self.position - home->position) < 200
			? m_world.titanToWorld(home->object, {0, 40, 0}).y : 0;
		station.y = std::max({clear, m_world.terrainHeight(station.x, station.z) + 30,
			m_world.terrainHeight(support->position.x, support->position.z) + 60 + 10 * self.team});
		if (bot.titanSupportUntil <= 0 && horizontal(station - self.position) < 40) {
			bot.titanSupportUntil = m_time + 20;
		}
		bot.behaviour = "TitanSupport";
		bot.titanFlyTo = station;
		bot.titanFlyMode = 4;
		return fly(bot, self, input, seconds);
	}
	bot.behaviour = "TitanSortie";
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
				boarders += p.alive && p.team == enemy->team && !p.vehicle && titanUnder(titans, p) == own;
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

// enhanced.botTitanRepair: whether a kit carries the repair tool (a child weapon of the Repair AI type).
bool BotManager::kitRepairs(const std::string& kit) const
{
	const world::ObjectTemplate* t = const_cast<GameWorld&>(m_world).templates().find(kit);
	if (!t) {
		return false;
	}
	for (const auto& child : t->children()) {
		if (weaponAi(child.name).type == WeaponAi::Repair) {
			return true;
		}
	}
	return false;
}

bool BotManager::titanWantsEngineer(int team)
{
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	const GameWorld::TitanInfo* own = titanOf(titans, team);
	if (!own || team < 1 || team > 2) {
		return false;
	}
	bool down = false;
	for (const GameWorld::TitanPartInfo& part : m_world.titanParts(team, own->object)) {
		down = down || (part.kind == GameWorld::TitanPartInfo::Gun && (part.wreck || part.hitPoints < 0.5f * part.maxHitPoints));
	}
	if (!down) {
		return false;
	}
	std::vector<float>& sent = m_engineersSent[static_cast<size_t>(team)];
	sent.erase(std::remove_if(sent.begin(), sent.end(), [this](float t) { return m_time - t > 20; }), sent.end());
	long engineers = static_cast<long>(sent.size());
	for (const GameWorld::PlayerInfo& p : m_players) {
		engineers += p.alive && p.team == team && !p.vehicle && aboard(*own, p.position) && kitSlot(p, WeaponAi::Repair) != 0;
	}
	return engineers < 2;
}

// Where a bot with a role spawns: a defender on its Titan; a boarder in a
// spawn vehicle of the team within 200 m of the enemy's Titan (a transport
// parked on its deck, an APC beside it), else on its own Titan while the
// transport there has room; any other while enemies are aboard its Titan
// and it has fewer than two on foot there for each; enhanced.botTitanRepair:
// one with the repair tool (chooseRespawn gave it the kit) aboard while its
// Titan wants an engineer (titanWantsEngineer). False: the SAI's choice
// stands.
bool BotManager::titanRespawn(const GameWorld::PlayerInfo& self)
{
	const int role = titanRole(self);
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	const GameWorld::TitanInfo* own = titanOf(titans, self.team);
	const GameWorld::TitanInfo* enemy = titanOf(titans, 3 - self.team);
	if (!own || !enemy) {
		return false;
	}
	// enhanced.botTitanRepair: an engineer spawns aboard to mend a downed gun (chooseRespawn gave it the kit).
	if (m_settings.titanRepairs() && role != 1 && titanWantsEngineer(self.team)) {
		const auto r = m_respawn.find(self.id);
		const auto& kits = m_teamKits[static_cast<size_t>(self.team)];
		if (r != m_respawn.end() && r->second.kit >= 0 && static_cast<size_t>(r->second.kit) < kits.size()
			&& kitRepairs(kits[static_cast<size_t>(r->second.kit)].first)) {
			m_engineersSent[static_cast<size_t>(self.team)].push_back(m_time);
			m_world.setSpawnTitan(self.id, own->object);
			return true;
		}
	}
	if (!role) {
		if (!m_settings.titanPlay()) {
			return false;
		}
		// Enemies aboard: the rest come back to its Titan too, while it has
		// fewer than two on foot aboard for each of them.
		int boarders = 0, defenders = 0;
		for (const GameWorld::PlayerInfo& p : m_players) {
			if (p.alive && !p.vehicle && titanUnder(titans, p) == own) {
				boarders += p.team == enemy->team;
				defenders += p.team == self.team;
			}
		}
		if (!boarders || defenders >= 2 * boarders) {
			return false;
		}
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
	TP("wayToVehicleMesh");
	constexpr float kGrid = 4;
	constexpr int kHalf = 22; // 88 m
	const std::pair<int, int> key {static_cast<int>(std::floor(from.x / kGrid)), static_cast<int>(std::floor(from.z / kGrid))};
	// The search walks the terrain: nothing for a vehicle parked above it (a Titan's deck, a roof).
	static const std::vector<physics::Vec3> none;
	if (from.y - m_world.terrainHeight(from.x, from.z) > 5) {
		return none;
	}
	const auto known = m_meshWays.find(key);
	// (A search that found no way costs up to 50 ms: not made again for a minute.)
	if (known != m_meshWays.end() && (!known->second.second.empty() || m_time - known->second.first < 60)) {
		return known->second.second;
	}
	if (!m_vehicleNavMesh) {
		m_meshWays[key] = {m_time, {}};
		return m_meshWays[key].second;
	}
	// enhanced.botAsyncPaths: one search at a time, kMeshRaysPerTick rays a tick (a failing one casts some
	// 65000): an unfinished search goes on when its spot is asked for again, other spots get none meanwhile
	// (and one not asked for in 5 s is dropped). Without it the whole search is made at once.
	const bool spread = m_settings.asyncPaths();
	MeshSearch& s = m_meshSearch;
	if (s.active && s.key != key) {
		if (spread && m_time - s.askedAt <= 5) {
			return none;
		}
		s.active = false;
	}
	const int side = 2 * kHalf + 1;
	const int start = kHalf * side + kHalf;
	if (!s.active) {
		s.active = true;
		s.key = key;
		s.from = from;
		s.ignore = ignore;
		s.seconds = 0;
		s.cameFrom.assign(static_cast<size_t>(side * side), -1);
		s.frontier = {};
		s.cameFrom[static_cast<size_t>(start)] = start;
		s.frontier.push(start);
	}
	s.askedAt = m_time;
	const auto t0 = std::chrono::steady_clock::now();
	const physics::Vec3 origin = s.from;
	const auto at = [&](int i) {
		physics::Vec3 p {origin.x + (i % side - kHalf) * kGrid, 0, origin.z + (i / side - kHalf) * kGrid};
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
				--m_meshRaysLeft;
				if (!m_world.lineOfSight(a + lift, b + lift, s.ignore, through) || through) {
					return false;
				}
			}
		}
		return std::fabs(d.y) < 0.6f * length; // not up a wall of terrain
	};
	int goal = -1;
	while (!s.frontier.empty() && goal < 0 && (!spread || m_meshRaysLeft > 0)) {
		const int c = s.frontier.front();
		s.frontier.pop();
		const physics::Vec3 here = c == start ? physics::Vec3 {origin.x, m_world.terrainHeight(origin.x, origin.z), origin.z} : at(c);
		for (int dz = -1; dz <= 1 && goal < 0; ++dz) {
			for (int dx = -1; dx <= 1; ++dx) {
				const int x = c % side + dx, z = c / side + dz, n = z * side + x;
				if ((!dx && !dz) || x < 0 || z < 0 || x >= side || z >= side || s.cameFrom[static_cast<size_t>(n)] >= 0 || !open(here, at(n))) {
					continue;
				}
				s.cameFrom[static_cast<size_t>(n)] = c;
				const physics::Vec3 p = at(n);
				if (m_vehicleNavMesh->findPolygon(ai::Vec3 {p.x, p.y, p.z}) >= 0) {
					goal = n;
					break;
				}
				s.frontier.push(n);
			}
		}
	}
	const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	s.seconds += took;
	m_groundStats.vehicleMaxSeconds = std::max(m_groundStats.vehicleMaxSeconds, took);
	if (goal < 0 && !s.frontier.empty()) {
		return none; // (the rest next tick)
	}
	s.active = false;
	std::vector<physics::Vec3>& way = m_meshWays[key].second;
	m_meshWays[key].first = m_time;
	way.clear();
	for (int c = goal; c >= 0 && c != start; c = s.cameFrom[static_cast<size_t>(c)]) {
		way.insert(way.begin(), at(c));
	}
	++m_groundStats.vehicleSearches;
	m_groundStats.vehicleSeconds += s.seconds;
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
	physics::Vec3 aim = bot.meshWay.front();
	if (m_settings.offMeshWalk()) {
		// enhanced.botOffMesh: a car can't turn onto a point 4 m off (the grid's steps), so it ran past the way's
		// next point, turned back for it and wandered (Cerbere_Landing gpm_cq 32: PAC's FAV at the Harbor, 74 m
		// from the vehicle mesh, drove about for the minute and was given up). The way up to the step nearest
		// the vehicle counts as gone by, and it steers for the farthest step within 12 m ahead.
		size_t nearest = 0;
		for (size_t i = 1; i < bot.meshWay.size(); ++i) {
			if (horizontal(bot.meshWay[i] - here) < horizontal(bot.meshWay[nearest] - here)) {
				nearest = i;
			}
		}
		bot.meshWay.erase(bot.meshWay.begin(), bot.meshWay.begin() + static_cast<std::ptrdiff_t>(nearest));
		size_t ahead = 0;
		for (size_t i = 0; i < bot.meshWay.size() && horizontal(bot.meshWay[i] - here) <= 12; ++i) {
			ahead = i;
		}
		aim = bot.meshWay[ahead];
		// The last step is the mesh's edge: on past it, 8 m on along the way's last leg (else it circles the edge).
		if (ahead + 1 == bot.meshWay.size()) {
			const physics::Vec3 from = bot.meshWay.size() > 1 ? bot.meshWay[ahead - 1] : here;
			const physics::Vec3 leg = aim - from;
			const float length = horizontal(leg);
			if (length > 0.5f) {
				aim = aim + physics::Vec3 {leg.x, 0, leg.z} * (8 / length);
			}
		}
	}
	const physics::Vec3 d = aim - here;
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

bool BotManager::onTerrain(const physics::Vec3& at) const
{
	return at.y - m_world.terrainHeight(at.x, at.z) < 3;
}

bool BotManager::meshExit(const physics::Vec3& to, physics::Vec3& exit)
{
	constexpr float kGrid = 4;
	const std::pair<int, int> key {static_cast<int>(std::floor(to.x / kGrid)), static_cast<int>(std::floor(to.z / kGrid))};
	if (const auto known = m_meshExits.find(key); known != m_meshExits.end()) {
		exit = known->second.second;
		return known->second.first;
	}
	bool found = false;
	for (float r = kGrid; r <= 200 && !found && m_navMesh; r += kGrid) {
		const int n = std::max(8, static_cast<int>(6.2831853f * r / kGrid));
		for (int k = 0; k < n && !found; ++k) {
			const float a = 6.2831853f * static_cast<float>(k) / static_cast<float>(n);
			physics::Vec3 p = to + physics::Vec3 {std::sin(a) * r, 0, std::cos(a) * r};
			p.y = m_world.terrainHeight(p.x, p.z);
			if (m_navMesh->findPolygon(ai::Vec3 {p.x, p.y, p.z}) >= 0) {
				exit = p;
				found = true;
			}
		}
	}
	m_meshExits[key] = {found, exit};
	return found;
}

std::vector<physics::Vec3> BotManager::groundWay(const physics::Vec3& from, const physics::Vec3& to, bool toMesh, uint32_t ignore,
	bool* unfinished, bool offset) const
{
	TP("groundWay");
	if (unfinished) {
		*unfinished = false;
	}
	int newSteps = 0;
	constexpr float kGrid = 4, kMargin = 80, kReach = 5;
	constexpr int kMaxExpanded = 12000;
	// The cells lie on the world's 4 m grid (the start is `from` itself, in the cell nearest it), or with
	// `offset` on that grid moved half a cell (a gap the one grid's steps cut across obliquely the other
	// may pass), so the steps' checks are kept for the next searches (m_groundEdges), for a minute: a
	// vehicle parked across a step moves on.
	if (m_time - m_groundEdgesAt > 60) {
		m_groundEdges.clear();
		m_groundEdgesAt = m_time;
	}
	const float shift = offset ? kGrid / 2 : 0;
	const int ox = static_cast<int>(std::lround((from.x - shift) / kGrid)), oz = static_cast<int>(std::lround((from.z - shift) / kGrid));
	const float cx = static_cast<float>(ox) * kGrid + shift, cz = static_cast<float>(oz) * kGrid + shift;
	const int x0 = static_cast<int>(std::floor((std::min(from.x, to.x) - kMargin - cx) / kGrid));
	const int x1 = static_cast<int>(std::ceil((std::max(from.x, to.x) + kMargin - cx) / kGrid));
	const int z0 = static_cast<int>(std::floor((std::min(from.z, to.z) - kMargin - cz) / kGrid));
	const int z1 = static_cast<int>(std::ceil((std::max(from.z, to.z) + kMargin - cz) / kGrid));
	const int w = x1 - x0 + 1, h = z1 - z0 + 1;
	const auto cellAt = [&](int i) {
		physics::Vec3 p {cx + static_cast<float>(i % w + x0) * kGrid, 0, cz + static_cast<float>(i / w + z0) * kGrid};
		p.y = m_world.terrainHeight(p.x, p.z);
		return p;
	};
	const auto edgeKey = [&](int c, int dx, int dz) {
		const uint64_t gx = static_cast<uint32_t>(ox + c % w + x0) & 0xffffffu, gz = static_cast<uint32_t>(oz + c / w + z0) & 0xffffffu;
		return static_cast<uint64_t>(offset) << 52 | gx << 28 | gz << 4 | static_cast<uint64_t>((dz + 1) * 3 + dx + 1);
	};
	float water = -kInfinity;
	m_world.waterLevel(water);
	// A step is open when nothing crosses it at a soldier's knee and chest, it isn't up a slope over 40
	// degrees (the infantry map's maxSlope) and it doesn't end in water.
	const auto open = [&](const physics::Vec3& a, const physics::Vec3& b) {
		const physics::Vec3 d = b - a;
		if (b.y < water - 0.5f || std::fabs(d.y) > 0.84f * horizontal(d)) {
			return false;
		}
		const physics::Vec3 across = physics::Vec3 {d.z, 0, -d.x} * (0.5f / std::max(horizontal(d), 0.01f));
		for (const float aside : {0.0f, 1.0f, -1.0f}) {
			for (const float up : {0.7f, 1.6f}) {
				bool through = false;
				const physics::Vec3 lift = across * aside + physics::Vec3 {0, up, 0};
				if (!m_world.lineOfSight(a + lift, b + lift, ignore, through) || through) {
					return false;
				}
			}
		}
		return true;
	};
	const size_t cells = static_cast<size_t>(w) * static_cast<size_t>(h);
	std::vector<float> cost(cells, kInfinity);
	std::vector<int> cameFrom(cells, -1);
	std::vector<bool> closed(cells, false);
	using Entry = std::pair<float, int>;
	std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> frontier;
	const int start = (0 - z0) * w + (0 - x0);
	cost[static_cast<size_t>(start)] = 0;
	frontier.push({horizontal(to - from), start});
	int goal = -1, expanded = 0;
	while (!frontier.empty() && goal < 0 && expanded < kMaxExpanded) {
		const int c = frontier.top().second;
		frontier.pop();
		if (closed[static_cast<size_t>(c)]) {
			continue;
		}
		closed[static_cast<size_t>(c)] = true;
		++expanded;
		const physics::Vec3 here = c == start ? physics::Vec3 {from.x, m_world.terrainHeight(from.x, from.z), from.z} : cellAt(c);
		if (c != start && (horizontal(here - to) <= kReach || (toMesh && m_navMesh && m_navMesh->findPolygon(ai::Vec3 {here.x, here.y, here.z}) >= 0))) {
			goal = c;
			break;
		}
		for (int dz = -1; dz <= 1; ++dz) {
			for (int dx = -1; dx <= 1; ++dx) {
				const int x = c % w + dx, z = c / w + dz;
				if ((!dx && !dz) || x < 0 || z < 0 || x >= w || z >= h) {
					continue;
				}
				const int n = z * w + x;
				const physics::Vec3 there = cellAt(n);
				const float step = cost[static_cast<size_t>(c)] + std::sqrt(physics::lengthSquared(there - here));
				if (closed[static_cast<size_t>(n)] || step >= cost[static_cast<size_t>(n)]) {
					continue;
				}
				bool through = false;
				if (c == start) {
					// (Soldiers aren't among the bodies lineOfSight meets, so `ignore` changes no kept check.)
					through = open(here, there);
				} else {
					const uint64_t key = edgeKey(c, dx, dz);
					const auto known = m_groundEdges.find(key);
					if (known != m_groundEdges.end()) {
						through = known->second;
					} else if (unfinished && newSteps >= kGroundNewSteps) {
						*unfinished = true;
						m_groundStats.cells += expanded;
						m_groundStats.steps += newSteps;
						m_groundStats.maxCells = std::max(m_groundStats.maxCells, expanded);
						return {};
					} else {
						++newSteps;
						through = m_groundEdges[key] = open(here, there);
					}
				}
				if (!through) {
					continue;
				}
				cost[static_cast<size_t>(n)] = step;
				cameFrom[static_cast<size_t>(n)] = c;
				frontier.push({step + horizontal(to - there), n});
			}
		}
	}
	m_groundStats.cells += expanded;
	m_groundStats.steps += newSteps;
	m_groundStats.maxCells = std::max(m_groundStats.maxCells, expanded);
	std::vector<physics::Vec3> way;
	for (int c = goal; c >= 0 && c != start; c = cameFrom[static_cast<size_t>(c)]) {
		way.insert(way.begin(), cellAt(c));
	}
	return way;
}

bool BotManager::groundWalk(Bot& bot, const GameWorld::PlayerInfo& self, const physics::Vec3& to, physics::Vec3& meshTo,
	physics::Vec3& point)
{
	if (!(m_settings.offMeshWalk() || (m_settings.titanPlay() && m_world.gameMode() == "gpm_ti")) || !m_navMesh || self.vehicle
		|| !onTerrain(self.position) || !onTerrain(to)) {
		bot.groundWay.clear();
		return false;
	}
	// The mesh's answers for the bot and its goal, looked up again after a metre.
	if (physics::lengthSquared(self.position - bot.groundSelfAt) > 1) {
		bot.groundSelfAt = self.position;
		bot.groundSelfOn = m_navMesh->findPolygon(ai::Vec3 {self.position.x, self.position.y, self.position.z}) >= 0;
	}
	if (physics::lengthSquared(to - bot.groundGoalAt) > 1) {
		bot.groundGoalAt = to;
		bot.groundGoalOn = m_navMesh->findPolygon(ai::Vec3 {to.x, to.y, to.z}) >= 0;
	}
	const bool selfOn = bot.groundSelfOn, goalOn = bot.groundGoalOn;
	if (selfOn && goalOn) {
		bot.groundWay.clear();
		return false;
	}
	if (selfOn) {
		// The mesh's way as far as it goes towards `to`, then the grid.
		physics::Vec3 exit;
		if (meshExit(to, exit) && horizontal(self.position - exit) > 10 && horizontal(self.position - to) > horizontal(exit - to) + 5) {
			meshTo = exit;
			bot.groundWay.clear();
			return false;
		}
	}
	if (horizontal(self.position - to) <= 6) {
		// There (a silo's capture sphere is 6 m): straight on at it, no search that could lead out again.
		bot.groundWay.clear();
		bot.groundTo = to;
		point = to;
		return true;
	}
	if (horizontal(self.position - bot.groundAt) > 3) {
		bot.groundAt = self.position;
		bot.groundAtTime = m_time;
	}
	// Searched again for another goal (2 s after the last search), after 6 s without 3 m made, or with no
	// way left short of it (3 s after the last search). A goal moving on the mesh (a squad member's spot
	// by its leader) keeps a way that ends on the mesh. After searches that found nothing the next waits
	// longer (groundRetryAt), unless the goal is another 50 m off.
	const float moved = horizontal(bot.groundTo - to);
	const bool other = moved > 5 && !(goalOn && bot.groundToMesh && !bot.groundWay.empty())
		&& m_time - bot.groundPlannedAt >= 2 && (m_time >= bot.groundRetryAt || moved > 50);
	if (moved <= 5) {
		bot.groundTo = to;
	}
	const bool stuck = m_time - bot.groundAtTime > 6;
	const bool spent = bot.groundWay.empty() && horizontal(self.position - to) > 5;
	if (other || ((stuck || spent) && m_time - bot.groundPlannedAt >= 3 && m_time >= bot.groundRetryAt)) {
		constexpr float kCell = 4;
		const auto cell = [](float v) { return static_cast<int>(std::floor(v / kCell)); };
		// After a search found nothing the next is made on the other grid (offset), then back.
		const bool offset = bot.groundFailures % 2 == 1;
		const GroundKey key {cell(self.position.x), cell(self.position.z), cell(to.x), cell(to.z), goalOn, offset};
		const auto known = m_groundWays.find(key);
		// A way kept for this start and goal, unless it is the one the bot got stuck on (none kept is
		// none again from the same cell).
		const bool reuse = known != m_groundWays.end() && (!stuck || other || known->second.empty());
		bool found = reuse;
		if (reuse) {
			bot.groundWay = known->second;
			++m_groundStats.reused;
		} else if (m_groundSearchesLeft > 0) {
			--m_groundSearchesLeft;
			const auto t0 = std::chrono::steady_clock::now();
			bool unfinished = false;
			std::vector<physics::Vec3> way = groundWay(self.position, to, goalOn, self.soldier, &unfinished, offset);
			const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
			m_groundStats.seconds += took;
			m_groundStats.maxSeconds = std::max(m_groundStats.maxSeconds, took);
			++m_groundStats.searches;
			if (unfinished) {
				// Its steps' checks kept: the search goes on at the next tick's turn, the old way till then.
				++m_groundStats.unfinished;
			} else {
				found = true;
				m_groundStats.empty += way.empty();
				if (m_groundWays.size() >= 1024) {
					m_groundWays.clear();
				}
				m_groundWays[key] = way;
				bot.groundWay = std::move(way);
			}
		} else {
			// Over this tick's searches: the old way a tick longer (asked again the next).
			++m_groundStats.deferred;
		}
		if (found) {
			bot.groundFailures = bot.groundWay.empty() ? std::min(bot.groundFailures + 1, 6) : 0;
			bot.groundRetryAt = bot.groundFailures ? m_time + 3.0f * static_cast<float>(1 << (bot.groundFailures - 1)) : m_time;
			bot.groundTo = to;
			bot.groundToMesh = goalOn;
			bot.groundPlannedAt = m_time;
			bot.groundAtTime = m_time;
			bot.groundAt = self.position;
		}
	}
	while (!bot.groundWay.empty() && horizontal(bot.groundWay.front() - self.position) < 2) {
		bot.groundWay.erase(bot.groundWay.begin());
	}
	point = bot.groundWay.empty() ? to : bot.groundWay.front();
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
	return out;
}

BotManager::TitanGridState BotManager::titanGridState(uint32_t titan, int team, uint32_t key) const
{
	TitanGridState s;
	const auto found = m_titanGrids.find({titan, team});
	if (found == m_titanGrids.end() || !found->second) {
		return s;
	}
	const TitanGrid& g = *found->second;
	s.exists = true;
	s.version = g.version;
	s.cells = g.cells.size();
	s.fields = g.fields.size();
	s.pending = g.pending.size();
	s.rebuilding = g.rebuild.job != nullptr;
	s.landingDone = g.landingDone;
	s.landingPending = g.landingJob.job != nullptr || g.landingStale;
	s.landingCells = g.landing.size();
	if (const auto field = g.fields.find(key); field != g.fields.end()) {
		s.hasField = true;
		for (size_t c = 0; c < field->second.size(); ++c) {
			if (field->second[c] < kInfinity) {
				++s.reached;
				s.fieldSum += static_cast<double>(field->second[c]) * static_cast<double>(c % 97 + 1);
			}
		}
	}
	return s;
}

// enhanced.botCommander (not retail): AIDLL's commander never moves a Titan
// (its IBFCommander calls are the strikes, the UAV and the supply drop,
// BotManager::command), and a Titan moves only on a commander's order
// (GameWorld::moveTitan). A team with bots and no commander gets a virtual
// one here, every kCommandEvery seconds:
//   - its focus: the enemy Titan once its shield is down (boarding wins);
//     else a silo the team holds that is being taken (the lowest flag
//     under 0.75, as command() reads "being taken"); else the silo most
//     of its bots are going for (the nearest to its Titan on a tie), kept
//     kKeepFocus seconds while it is not yet the team's;
//   - the Titan (while it may move: above 60 %) to the spot nearest that
//     focus, counting the way there, that leaves every friendly silo's
//     missile path clear of its box, the focus silo counted as friendly:
//     a missile climbs out of its silo and runs straight at the enemy's
//     middle, and a Titan in its way takes it (rounds meet every field);
//     the order is kept while the focus stays and its spot stays clear;
//   - the commander's tools at the focus: the UAV; supplies to two or
//     more of its soldiers on foot there; the orbital strike on two or
//     more enemies bunched there with no teammate within 40 m (command()'s
//     rule); the EMP on the known crewed enemy vehicle, anywhere on the
//     field, not an aircraft, that threatens the most of ours (parked ones are no threat, and
//     the Remaster 5 s recharge leaves nothing else to hold it back), at
//     most one a minute: it kept a disabled turret down every 10 s.
namespace {
constexpr float kCommandEvery = 10;
constexpr float kKeepFocus = 90;     // shortcut: a fixed hold; the Titan crosses ~95 m in it
constexpr float kMissileClimb = 45;  // a silo missile's climb before it runs at the Titan (Suez, measured: 45 m up, turned within 40 m)
constexpr float kSiloClearance = 10; // round the Titan's box
constexpr float kEmpEvery = 60;      // shortcut: the bots' own pace (Remaster's EMP is ready in 5 s; vanilla 115 s), tune by feel
} // namespace

bool BotManager::titanBlocksSilos(const GameWorld::TitanInfo& own, const GameWorld::TitanInfo& enemy, const physics::Vec3& at,
	int extraPoint, const std::vector<GameWorld::SiloInfo>& silos) const
{
	physics::Vec3 low, high;
	if (!m_world.objectBox(own.object, low, high)) {
		return false;
	}
	const physics::Vec3 margin {kSiloClearance, kSiloClearance, kSiloClearance};
	low = low - margin;
	high = high + margin;
	// The Titan moved to `at` (it never turns): a point there is where this one has it.
	const physics::Vec3 shift {own.position.x - at.x, 0, own.position.z - at.z};
	const auto inside = [&](const physics::Vec3& q) {
		const physics::Vec3 l = m_world.titanToLocal(own.object, q + shift);
		return l.x > low.x && l.x < high.x && l.y > low.y && l.y < high.y && l.z > low.z && l.z < high.z;
	};
	for (const GameWorld::SiloInfo& silo : silos) {
		if (silo.team != own.team && silo.controlPoint != extraPoint) {
			continue;
		}
		const physics::Vec3 top = silo.position + physics::Vec3 {0, kMissileClimb, 0};
		const int steps = std::max(1, static_cast<int>(std::sqrt(physics::lengthSquared(enemy.position - top)) / 10));
		for (int i = 0; i <= 4; ++i) {
			if (inside(silo.position + (top - silo.position) * (i / 4.0f))) {
				return true;
			}
		}
		for (int i = 0; i <= steps; ++i) {
			if (inside(top + (enemy.position - top) * (static_cast<float>(i) / steps))) {
				return true;
			}
		}
	}
	return false;
}

bool BotManager::titanOrderBlocksSilos(int team, const physics::Vec3& at) const
{
	if (team < 1 || team > 2) {
		return false;
	}
	const std::vector<GameWorld::TitanInfo> titans = m_world.titans();
	const GameWorld::TitanInfo* own = titanOf(titans, team);
	const GameWorld::TitanInfo* enemy = titanOf(titans, 3 - team);
	return own && enemy && titanBlocksSilos(*own, *enemy, at, m_titanCommands[team].focus, m_world.missileSilos());
}

void BotManager::titanCommand()
{
	TP("titanCommand");
	if (!m_settings.titanCommander() || m_world.gameMode() != "gpm_ti") {
		return;
	}
	std::vector<GameWorld::TitanInfo> titans;
	std::vector<GameWorld::SiloInfo> silos;
	std::vector<GameWorld::ControlPointInfo> points;
	for (int team = 1; team <= 2; ++team) {
		TitanCommand& c = m_titanCommands[team];
		if (m_time < c.nextAt) {
			continue;
		}
		c.nextAt = m_time + kCommandEvery;
		const bool bots = std::any_of(m_players.begin(), m_players.end(), [&](const GameWorld::PlayerInfo& p) { return p.ai && p.team == team; });
		if (!bots || m_world.commander(team) != -1) {
			continue;
		}
		if (titans.empty()) {
			titans = m_world.titans();
			silos = m_world.missileSilos();
			points = m_world.controlPointList();
		}
		const GameWorld::TitanInfo* own = titanOf(titans, team);
		const GameWorld::TitanInfo* enemy = titanOf(titans, 3 - team);
		if (!own || !enemy) {
			continue;
		}
		const auto pointOf = [&](int id) -> const GameWorld::ControlPointInfo* {
			for (const GameWorld::ControlPointInfo& p : points) {
				if (p.id == id) {
					return &p;
				}
			}
			return nullptr;
		};
		int focus = 0;
		physics::Vec3 at;
		if (!enemy->shield) {
			focus = -1;
			at = enemy->position;
		} else {
			float lowest = 0.75f;
			for (const ControlPoint& cp : m_world.controlPoints()) {
				const bool silo = std::any_of(silos.begin(), silos.end(), [&](const GameWorld::SiloInfo& s) { return s.controlPoint == cp.id; });
				if (silo && cp.object && cp.team == team && cp.flag == team && cp.flagHeight < lowest) {
					lowest = cp.flagHeight;
					focus = cp.id;
					at = {cp.object->position().x, cp.object->position().y, cp.object->position().z};
				}
			}
			if (!focus) {
				std::map<int, int> going; // by control point
				for (const auto& [id, bot] : m_bots) {
					const GameWorld::PlayerInfo* p = playerOf(id);
					if (p && p->team == team && bot.goalOrder >= 0 && static_cast<size_t>(bot.goalOrder) < m_settings.strategy.areas.size()) {
						++going[m_settings.strategy.areas[static_cast<size_t>(bot.goalOrder)].controlPoint];
					}
				}
				float best = -1;
				for (const GameWorld::SiloInfo& silo : silos) {
					const GameWorld::ControlPointInfo* point = pointOf(silo.controlPoint);
					if (silo.team == team || !point || !point->capturable) {
						continue;
					}
					// The silo it went for stays its focus for kKeepFocus seconds while not yet the team's.
					const bool kept = silo.controlPoint == c.focus && m_time < c.focusSince + kKeepFocus;
					const float score = (kept ? 1e9f : 0.0f) + static_cast<float>(going[silo.controlPoint]) * 10000.0f
						- horizontal(silo.position - own->position);
					if (focus == 0 || score > best) {
						best = score;
						focus = silo.controlPoint;
						at = silo.position;
					}
				}
			}
		}
		if (!focus) {
			continue;
		}
		if (focus != c.focus) {
			c.focusSince = m_time;
			c.focus = focus;
			c.ordered = false; // a new focus: a new order
		}
		titanCommandAssets(team, at, focus > 0);
		if (!own->canMove) {
			continue;
		}
		const float apart = own->radius + enemy->radius;
		const float ring = apart + 30;
		if (c.ordered && !titanBlocksSilos(*own, *enemy, c.target, focus, silos)
			&& (focus > 0 || std::fabs(horizontal(c.target - enemy->position) - ring) < 60)) {
			continue;
		}
		// The spots: about the silo (over it, 60 m and 120 m off), or a ring beside the enemy's Titan.
		std::vector<std::pair<float, physics::Vec3>> spots;
		const std::vector<float> radii = focus > 0 ? std::vector<float> {0, 60, 120} : std::vector<float> {ring, ring + 50};
		for (const float r : radii) {
			for (int i = 0; i < (r > 0 ? 16 : 1); ++i) {
				const float a = static_cast<float>(i) * 6.2831853f / 16;
				const physics::Vec3 p {at.x + std::sin(a) * r, own->position.y, at.z + std::cos(a) * r};
				if (horizontal(p - enemy->position) < apart + 20 || titanBlocksSilos(*own, *enemy, p, focus, silos)) {
					continue;
				}
				// Crossing a missile's path on the way costs a shot or two.
				bool crosses = false;
				for (int k = 1; k < 5 && !crosses; ++k) {
					crosses = titanBlocksSilos(*own, *enemy, own->position + (p - own->position) * (k / 5.0f), focus, silos);
				}
				spots.push_back({horizontal(p - at) + 0.3f * horizontal(p - own->position) + (crosses ? 400.0f : 0.0f), p});
			}
		}
		std::sort(spots.begin(), spots.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		for (const auto& [score, p] : spots) {
			const GameWorld::TitanMove answer = m_world.moveTitan(team, p.x, p.z);
			if (answer == GameWorld::TitanMove::Accepted) {
				c.ordered = true;
				c.target = p;
				if (std::getenv("BF2142_BOT_STATS")) {
					std::printf("commander %s %.0f s: Titan at %.0f/%.0f to %.0f/%.0f for %s %d\n", team == 1 ? "PAC" : "EU", m_time,
						own->position.x, own->position.z, p.x, p.z, focus > 0 ? "point" : "the enemy Titan", focus);
				}
				break;
			}
			if (answer != GameWorld::TitanMove::Unable) {
				c.nextAt = std::min(c.nextAt, m_time + 1); // too soon: again in a second
				break;
			}
		}
	}
}

void BotManager::titanCommandAssets(int team, const physics::Vec3& focus, bool silo)
{
	using A = GameWorld::CommanderAbility;
	const auto use = [&](A a, const physics::Vec3& p) {
		if (!m_world.abilityState(team, a).ready || !m_world.commanderAction(team, a, p.x, p.z, true)) {
			return false;
		}
		if (std::getenv("BF2142_BOT_STATS")) {
			static const char* const names[] = {"EMP", "orbital strike", "UAV", "satellite", "supplies"};
			std::printf("commander %s %.0f s: %s at %.0f/%.0f\n", team == 1 ? "PAC" : "EU", m_time, names[static_cast<int>(a)], p.x, p.z);
		}
		return true;
	};
	const auto count = [&](int side, const physics::Vec3& p, float r, bool onFoot) {
		return std::count_if(m_players.begin(), m_players.end(), [&](const GameWorld::PlayerInfo& q) {
			return q.alive && q.team == side && (!onFoot || !q.vehicle) && horizontal(q.position - p) < r;
		});
	};
	use(A::Uav, focus);
	// Not while a crate of the team's, landed or still coming down, is there: a crate leaves the world's list once
	// spent, expired or destroyed (destroyWhenEmptyStorage in both mods), so a listed one still serves.
	const std::vector<uint32_t> crates = m_world.supplyCrates();
	const bool supplied = std::any_of(crates.begin(), crates.end(), [&](uint32_t id) {
		const world::Object* o = m_world.objects().find(id);
		const world::Vec3 p = o ? o->position() : world::Vec3 {};
		return o && o->team() == team && horizontal(physics::Vec3 {p.x, p.y, p.z} - focus) < 30;
	});
	if (silo && !supplied && count(team, focus, 30, true) >= 2 && use(A::Supplies, focus) && std::getenv("BF2142_BOT_STATS")) {
		std::printf("commander %s %.0f s: supply crate %u\n", team == 1 ? "PAC" : "EU", m_time, m_world.supplyCrates().back());
	}
	physics::Vec3 sum;
	int enemies = 0;
	for (const GameWorld::PlayerInfo& q : m_players) {
		if (silo && q.alive && q.team == 3 - team && horizontal(q.position - focus) < 40) {
			sum = sum + q.position;
			++enemies;
		}
	}
	// Not in Titan mode: the commander screen shows the Titan's move where the artillery strike's button would be
	// (HudElementsCommanderComm.con, GameMode 1 or 3), so a human commander has no strike there.
	if (enemies >= 2 && m_world.gameMode() != "gpm_ti") {
		const physics::Vec3 middle = sum * (1.0f / static_cast<float>(enemies));
		if (count(3 - team, middle, 15, false) >= 2 && count(team, middle, 40, false) == 0) {
			use(A::Artillery, middle); // commander_orbital_strike (RCArtillery)
		}
	}
	// The EMP: of the crewed enemy vehicles its side knows of, anywhere on the field, the one that threatens most:
	// ours within 60 m of it, its crew, the focus counting 2 more; none of its own vehicles within 40 m.
	const auto teamOf = [&](uint32_t object) {
		const world::Object* o = m_world.objects().find(object);
		return o ? o->team() : 0;
	};
	std::vector<std::pair<uint32_t, physics::Vec3>> crewed; // vehicle, where its side knows one of its crew
	for (const GameWorld::PlayerInfo& q : m_players) {
		physics::Vec3 where;
		const world::Object* ride = q.vehicle ? m_world.objects().find(q.vehicle) : nullptr;
		if (q.alive && q.team == 3 - team && ride && ride->objectTemplate()
			&& !(m_settings.typeFlags(aiTemplateOf(ride->objectTemplate()->name())) & kITAir) // not aircraft
			&& m_knowledge[team].count(q.id) && knownPosition(team, q.id, where)) {
			crewed.push_back({q.vehicle, where});
		}
	}
	long best = 0;
	physics::Vec3 target;
	for (const auto& [vehicle, where] : crewed) {
		const auto v = std::find_if(m_vehicles.begin(), m_vehicles.end(), [&](const GameWorld::VehicleInfo& v) { return v.object == vehicle; });
		const long threatened = count(team, where, 60, false);
		const bool atFocus = horizontal(where - focus) < 60;
		if (v == m_vehicles.end() || !v->working || (!threatened && !atFocus)
			|| std::any_of(m_vehicles.begin(), m_vehicles.end(), [&](const GameWorld::VehicleInfo& mine) {
				   return teamOf(mine.object) == team && horizontal(mine.position - where) < 40;
			   })) {
			continue;
		}
		const long crew = std::count_if(crewed.begin(), crewed.end(), [&](const auto& c) { return c.first == vehicle; });
		const long score = threatened + crew + (atFocus ? 2 : 0);
		if (score > best) {
			best = score;
			target = where;
		}
	}
	if (best && m_time - m_titanCommands[team].empAt >= kEmpEvery && use(A::OrbitalStrike, target)) { // commander_emp_strike (RCOrbitalStrike)
		m_titanCommands[team].empAt = m_time;
	}
}

} // namespace dice::hfe::game
