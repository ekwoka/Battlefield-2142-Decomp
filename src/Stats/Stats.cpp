#include "Stats.h"

#include "Console/ConsoleBinding.h"
#include "World/ObjectTemplateManager.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dice::hfe::stats {

// --- Ranks

RankTable::RankTable()
{
	// Settings/RankSettings.con as shipped (1.51), until it is run.
	static const int shipped[] = {0, 40, 80, 120, 200, 330, 520, 750, 1050, 1400, 1800, 2250, 2850, 3550, 4400, 5300,
		6250, 7250, 8250, 9300, 10400, 11550, 12700, 14000, 15300, 16700, 18300, 20100, 22100, 24200, 26400, 28800,
		31500, 34200, 37100, 40200, 43300, 46900, 50500, 54100, 57700};
	for (int rank = 0; rank < static_cast<int>(sizeof(shipped) / sizeof(shipped[0])); ++rank) {
		m_points[rank] = shipped[rank];
	}
}

void RankTable::setRank(int rank, int points)
{
	if (rank >= 0) {
		m_points[rank] = points;
	}
}

int RankTable::maxRank() const
{
	int top = 0;
	for (const auto& [rank, points] : m_points) {
		if (rank > 0 && points > 0) {
			top = std::max(top, rank);
		}
	}
	return top;
}

int RankTable::points(int rank) const
{
	if (rank <= 0) {
		return 0;
	}
	const int top = maxRank();
	const auto it = m_points.find(std::min(rank, top));
	return it == m_points.end() ? 0 : it->second;
}

int RankTable::rankFor(int points) const
{
	int best = 0;
	for (const auto& [rank, needed] : m_points) {
		if (rank > 0 && needed > 0 && points >= needed && rank > best) {
			best = rank;
		}
	}
	return best;
}

float RankTable::progress(int points) const
{
	const int rank = rankFor(points);
	if (rank >= maxRank()) {
		return 1.0f;
	}
	const int from = this->points(rank), to = this->points(rank + 1);
	return to > from ? std::clamp(static_cast<float>(points - from) / static_cast<float>(to - from), 0.0f, 1.0f) : 1.0f;
}

RankTable& ranks()
{
	static RankTable table;
	return table;
}

void registerRankConsoleMethods(Console& console, RankTable& table)
{
	// 0x0043ac80 runs the file; the methods' table is a map rank -> points.
	bindMethod(console, "rankSettings.setRank", ConsoleAccess::Anyone,
		[&table](int rank, int points) { table.setRank(rank, points); });
}

// --- Unlocks

int unlockKit(int id) { return id / 100; }
int unlockBranch(int id) { return id / 10 % 10; }
int unlockLevel(int id) { return id % 10; }

int branchLength(int kit, int branch)
{
	if (kit >= 1 && kit <= 4 && (branch == 1 || branch == 2)) {
		return 5;
	}
	if (kit == 5) {
		// 0x0045c030: 6 player upgrades, 4 squad leader ones.
		return branch == 1 ? 6 : branch == 2 ? 4 : 0;
	}
	return 0;
}

bool isUnlockId(int id)
{
	const int level = unlockLevel(id);
	return id >= 100 && id < 600 && level >= 1 && level <= branchLength(unlockKit(id), unlockBranch(id));
}

bool isBoosterUnlock(int id)
{
	// 0x1004e000 / 0x1004b250: level 5 of the kit branches, 516 and 524.
	return isUnlockId(id) && ((id < 500 && id % 10 > 4) || id == 516 || id == 524);
}

std::vector<int> unlockIds(bool booster)
{
	std::vector<int> ids;
	for (int kit = 1; kit <= 5; ++kit) {
		for (int branch = 1; branch <= 2; ++branch) {
			for (int level = 1; level <= branchLength(kit, branch); ++level) {
				const int id = kit * 100 + branch * 10 + level;
				if (booster || !isBoosterUnlock(id)) {
					ids.push_back(id);
				}
			}
		}
	}
	return ids;
}

// --- A soldier

int Soldier::rank() const { return m_ranks.rankFor(m_stats.experience); }

int64_t localProfileId(const std::string& soldier)
{
	uint32_t h = 2166136261u;
	for (char c : soldier) {
		h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
	}
	return 0x10000000 | (h & 0x0fffffff);
}

std::map<std::string, std::string> playerInfoKeys(const SoldierStats& stats, const RankTable& ranks)
{
	return {{"rnk", std::to_string(ranks.rankFor(stats.experience))}};
}

int Soldier::nextRankPoints() const { return m_ranks.points(rank() + 1); }

float Soldier::rankProgress() const { return m_ranks.progress(m_stats.experience); }

int Soldier::credits() const { return std::max(0, rank() - unlocksSelected()); }

int Soldier::unlocksTotal() const
{
	// 0x1004b190: 40, 50 with the booster.
	return static_cast<int>(unlockIds(m_booster).size());
}

int Soldier::branchLevel(int kit, int branch) const
{
	int level = 0;
	for (const int id : m_stats.unlocks) {
		if (unlockKit(id) == kit && unlockBranch(id) == branch) {
			level = std::max(level, unlockLevel(id));
		}
	}
	return level;
}

UnlockStatus Soldier::status(int id) const
{
	if (!isUnlockId(id)) {
		return kUnlockLocked;
	}
	const int owned = branchLevel(unlockKit(id), unlockBranch(id));
	UnlockStatus status = unlockLevel(id) <= owned ? kUnlockTaken
		: unlockLevel(id) == owned + 1                ? kUnlockAvailable
													  : kUnlockLocked;
	if (status == kUnlockAvailable && credits() == 0) {
		status = kUnlockLocked;
	}
	if (isBoosterUnlock(id) && !m_booster) {
		status = kUnlockLocked;
	}
	return status;
}

SelectResult Soldier::select(int id)
{
	if (!isUnlockId(id) || (isBoosterUnlock(id) && !m_booster)) {
		return kSelectInvalid;
	}
	const int owned = branchLevel(unlockKit(id), unlockBranch(id));
	if (unlockLevel(id) <= owned) {
		return kSelectAlreadySelected;
	}
	if (unlockLevel(id) != owned + 1) {
		return kSelectUnavailable;
	}
	if (credits() == 0) {
		return kSelectNoCredits;
	}
	m_stats.unlocks.push_back(id);
	return kSelectDone;
}

bool Soldier::record(const RoundResult& round)
{
	const int before = rank();
	const int score = std::max(0, round.score);
	m_stats.experience += score;
	m_stats.kills += round.kills;
	m_stats.deaths += round.deaths;
	m_stats.rounds += 1;
	m_stats.timePlayed += std::max(0.0f, round.seconds);
	m_stats.bestRoundScore = std::max(m_stats.bestRoundScore, score);
	if (round.outcome > 0) {
		++m_stats.wins;
	} else if (round.outcome < 0) {
		++m_stats.losses;
	}
	return rank() > before;
}

// --- The store

std::string StatsStore::defaultFile()
{
	std::filesystem::path dir;
#if defined(_WIN32)
	if (const char* appData = std::getenv("APPDATA")) {
		dir = appData;
	}
#elif defined(__APPLE__)
	if (const char* home = std::getenv("HOME")) {
		dir = std::filesystem::path(home) / "Library" / "Application Support";
	}
#else
	if (const char* config = std::getenv("XDG_CONFIG_HOME")) {
		dir = config;
	} else if (const char* home = std::getenv("HOME")) {
		dir = std::filesystem::path(home) / ".config";
	}
#endif
	return dir.empty() ? std::string() : (dir / "bf2142-recomp" / "stats.txt").string();
}

void StatsStore::setFile(const std::string& path)
{
	m_file = path;
	m_soldiers.clear();
	if (path.empty()) {
		return;
	}
	std::ifstream in(path);
	std::stringstream text;
	text << in.rdbuf();
	read(text.str());
}

SoldierStats& StatsStore::soldier(const std::string& name)
{
	SoldierStats& stats = m_soldiers[name];
	stats.name = name;
	return stats;
}

const SoldierStats* StatsStore::find(const std::string& name) const
{
	const auto it = m_soldiers.find(name);
	return it == m_soldiers.end() ? nullptr : &it->second;
}

std::string StatsStore::write() const
{
	std::ostringstream out;
	for (const auto& [name, s] : m_soldiers) {
		out << "soldier " << name << "\n";
		out << "experience " << s.experience << "\n";
		out << "kills " << s.kills << "\n";
		out << "deaths " << s.deaths << "\n";
		out << "rounds " << s.rounds << "\n";
		out << "wins " << s.wins << "\n";
		out << "losses " << s.losses << "\n";
		out << "time " << static_cast<long long>(s.timePlayed) << "\n";
		out << "best " << s.bestRoundScore << "\n";
		out << "unlocks";
		for (const int id : s.unlocks) {
			out << " " << id;
		}
		out << "\n";
	}
	return out.str();
}

void StatsStore::read(const std::string& text)
{
	std::istringstream in(text);
	std::string line;
	SoldierStats* current = nullptr;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		const size_t space = line.find(' ');
		const std::string key = line.substr(0, space);
		const std::string value = space == std::string::npos ? std::string() : line.substr(space + 1);
		if (key == "soldier") {
			current = value.empty() ? nullptr : &soldier(value);
			continue;
		}
		if (!current) {
			continue;
		}
		const int number = std::atoi(value.c_str());
		if (key == "experience") {
			current->experience = number;
		} else if (key == "kills") {
			current->kills = number;
		} else if (key == "deaths") {
			current->deaths = number;
		} else if (key == "rounds") {
			current->rounds = number;
		} else if (key == "wins") {
			current->wins = number;
		} else if (key == "losses") {
			current->losses = number;
		} else if (key == "time") {
			current->timePlayed = std::atof(value.c_str());
		} else if (key == "best") {
			current->bestRoundScore = number;
		} else if (key == "unlocks") {
			current->unlocks.clear();
			std::istringstream ids(value);
			int id = 0;
			while (ids >> id) {
				if (isUnlockId(id) && std::find(current->unlocks.begin(), current->unlocks.end(), id) == current->unlocks.end()) {
					current->unlocks.push_back(id);
				}
			}
		}
	}
}

bool StatsStore::save() const
{
	if (m_file.empty()) {
		return true;
	}
	std::error_code ignored;
	std::filesystem::create_directories(std::filesystem::path(m_file).parent_path(), ignored);
	std::ofstream out(m_file);
	out << write();
	return static_cast<bool>(out);
}

// --- Unlocks in the game

namespace {

std::string setting(const world::ObjectTemplate& t, const char* name)
{
	const auto it = t.settings().find(name);
	return it == t.settings().end() ? std::string() : it->second;
}

} // namespace

std::vector<UnlockContainer> unlockContainers(const world::ObjectTemplateManager& templates)
{
	std::vector<UnlockContainer> out;
	for (const auto& kit : templates.templates()) {
		if (!kit || kit->typeName() != "Kit" || setting(*kit, "unlockIndex").empty()) {
			continue;
		}
		const std::string index = setting(*kit, "unlockIndex");
		const int kitId = std::atoi(index.c_str()) + 1;
		for (const world::ChildTemplate& child : kit->children()) {
			const world::ObjectTemplate* container = templates.find(child.name);
			if (!container || container->typeName() != "ItemContainer") {
				continue;
			}
			const std::string levelText = setting(*container, "unlockLevel");
			const int level = levelText.empty() ? 0 : std::atoi(levelText.c_str());
			const std::string branchName = setting(*container, "unlockBranch");
			int kitPart = kitId, branch = 1;
			if (branchName == "UB_Second" || branchName == "1") {
				branch = 2;
			} else if (branchName == "UB_Player" || branchName == "2") {
				kitPart = 5;
			} else if (branchName == "UB_SquadLeader" || branchName == "3") {
				kitPart = 5;
				branch = 2;
			}
			const int id = kitPart * 100 + branch * 10 + level + 1;
			if (isUnlockId(id)) {
				out.push_back({id, kit->name(), container->name()});
			}
		}
	}
	return out;
}

std::vector<std::string> containersFor(const std::vector<UnlockContainer>& all, const std::vector<int>& unlocks)
{
	std::vector<std::string> out;
	for (const UnlockContainer& c : all) {
		if (std::find(unlocks.begin(), unlocks.end(), c.id) != unlocks.end()
			&& std::find(out.begin(), out.end(), c.container) == out.end()) {
			out.push_back(c.container);
		}
	}
	return out;
}

// --- The stats server's answers

std::string StatsResponse::Block::value(size_t row, const std::string& key) const
{
	if (row >= rows.size()) {
		return {};
	}
	for (size_t i = 0; i < keys.size(); ++i) {
		if (keys[i] == key) {
			return i < rows[row].size() ? rows[row][i] : std::string();
		}
	}
	return {};
}

StatsResponse parseStatsResponse(const std::string& text)
{
	// 0x005752b0 cuts the text into tokens at runs of tabs and newlines
	// (0x005761d0, 0x00576210: empty fields vanish).
	StatsResponse out;
	size_t at = 0;
	while (true) {
		const size_t start = text.find_first_not_of("\t\n", at);
		if (start == std::string::npos) {
			break;
		}
		at = text.find_first_of("\t\n", start);
		out.tokens.push_back(text.substr(start, at == std::string::npos ? std::string::npos : at - start));
		if (at == std::string::npos) {
			break;
		}
	}
	if (out.tokens.empty()) {
		return out;
	}
	// The answer starts with "O"; anything else failed. (The client reads
	// nothing more of an "E" answer; its code is kept here for the log.)
	out.ok = out.tokens[0] == "O";
	if (out.tokens[0] == "E") {
		out.errorCode = out.tokens.size() > 1 ? std::atoi(out.tokens[1].c_str()) : -1;
	}
	if (!out.ok && out.tokens[0] != "E") {
		return out;
	}
	// "H" starts a block, its keys follow up to the first "D"; each "D"
	// starts a row whose values fill the keys in order; "$" ends the
	// answer. Without a "$" the answer counts as failed (0x005752b0 returns
	// 0); the length between the two "$" is never checked.
	enum class Reading { Nothing, Keys, Values } reading = Reading::Nothing;
	for (size_t i = 1; i < out.tokens.size(); ++i) {
		const std::string& token = out.tokens[i];
		if (token == "$") {
			out.terminated = true;
			out.declaredLength = i + 1 < out.tokens.size() ? std::atoi(out.tokens[i + 1].c_str()) : -1;
			break;
		}
		if (token == "H") {
			out.blocks.push_back({});
			reading = Reading::Keys;
		} else if (token == "D" && reading != Reading::Nothing) {
			out.blocks.back().rows.emplace_back();
			reading = Reading::Values;
		} else if (reading == Reading::Keys) {
			out.blocks.back().keys.push_back(token);
		} else if (reading == Reading::Values && out.blocks.back().rows.back().size() < out.blocks.back().keys.size()) {
			out.blocks.back().rows.back().push_back(token);
		}
	}
	return out;
}

// 0x0045c030 calls 0x005752b0 in its mode 1, which reads by position, not
// by key: after the "O" it skips five tokens and takes the sixth as the
// player's id, five more and the twelfth as the unspent credits (stored at
// the unlock info's +0x18: Avcred). Then each data row's first value is an
// unlock id (split into kit, branch and level digits), but only for a
// non-zero player id.
UnlocksInfo parseUnlocksInfo(const StatsResponse& response)
{
	UnlocksInfo info;
	if (!response.ok || !response.terminated || response.tokens.size() <= 12) {
		return info;
	}
	info.credits = std::atoi(response.tokens[12].c_str());
	if (std::atoi(response.tokens[6].c_str()) == 0) {
		return info;
	}
	// The rows after the twelfth token: the parse proper starts at the next
	// "H", and each "D" row's first value counts.
	bool inBlock = false;
	for (size_t i = 13; i < response.tokens.size() && response.tokens[i] != "$"; ++i) {
		const std::string& token = response.tokens[i];
		if (token == "H") {
			inBlock = true;
		} else if (token == "D" && inBlock && i + 1 < response.tokens.size()) {
			const int id = std::atoi(response.tokens[i + 1].c_str());
			if (isUnlockId(id)) {
				info.unlocks.push_back(id);
			}
		}
	}
	return info;
}


std::vector<AwardDefinition> parseAwards(const std::string& script)
{
	std::vector<AwardDefinition> out;
	std::istringstream lines(script);
	for (std::string line; std::getline(lines, line);) {
		std::istringstream words(line);
		std::string command, id;
		if (!(words >> command >> id) || command != "awards.setData") {
			continue;
		}
		AwardDefinition award;
		award.id = id;
		award.number = std::atoi(id.c_str());
		const size_t underscore = id.find('_');
		award.level = underscore == std::string::npos ? 0 : std::atoi(id.c_str() + underscore + 1);
		// The quoted criteria.
		for (size_t at = line.find('"'); at != std::string::npos; at = line.find('"', at)) {
			const size_t end = line.find('"', at + 1);
			if (end == std::string::npos) {
				break;
			}
			const std::string text = line.substr(at + 1, end - at - 1);
			at = end + 1;
			std::vector<std::string> fields;
			std::istringstream parts(text);
			for (std::string field; std::getline(parts, field, ',');) {
				fields.push_back(field);
			}
			if (fields.size() < 4) {
				continue;
			}
			AwardCriterion c;
			const int flags = std::atoi(fields[0].c_str());
			c.global = (flags & 1) != 0;
			c.check = (flags & 4) != 0;
			c.whole = (flags & 8) != 0;
			c.textId = std::atoi(fields[1].c_str());
			std::istringstream keys(fields[2]);
			for (std::string key; std::getline(keys, key, ';');) {
				key.erase(std::remove(key.begin(), key.end(), ' '), key.end());
				if (!key.empty()) {
					c.keys.push_back(key);
				}
			}
			c.target = static_cast<float>(std::atof(fields[3].c_str()));
			award.criteria.push_back(std::move(c));
		}
		out.push_back(std::move(award));
	}
	return out;
}

double awardStatValue(const SoldierStats& soldier, const std::string& key)
{
	if (key == "klls") {
		return soldier.kills;
	}
	if (key == "dths") {
		return soldier.deaths;
	}
	if (key == "gsco") {
		return soldier.experience;
	}
	if (key == "wins") {
		return soldier.wins;
	}
	if (key == "lose") {
		return soldier.losses;
	}
	if (key == "tt") {
		return soldier.timePlayed;
	}
	if (key == "wlr") {
		return soldier.losses > 0 ? static_cast<double>(soldier.wins) / soldier.losses : soldier.wins;
	}
	return 0;
}

} // namespace dice::hfe::stats
