#include "GameLogic.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

#include <cstdio>
#include <algorithm>
#include <cmath>

namespace dice::hfe::game {

GameLogic::GameLogic() = default;

Team& GameLogic::team(int index) { return m_teams[std::clamp(index, 0, 2)]; }
const Team& GameLogic::team(int index) const { return m_teams[std::clamp(index, 0, 2)]; }

void GameLogic::startRound()
{
	for (int t = 1; t <= 2; ++t) {
		Team& team = m_teams[t];
		const auto it = team.defaultTickets.find(mapSize);
		team.tickets = it != team.defaultTickets.end() ? static_cast<float>(it->second) : 0;
		team.ticketChangePerSecond = 0;
		team.ticketState = 0;
		team.ticketLimits.clear();
	}
	elapsed = 0;
	m_ended = false;
	m_winner = 0;
	m_victoryType = 0;
	m_timeLimitRaised = false;
}

void GameLogic::checkLimits(int teamIndex, float before, float after)
{
	if (!m_events) {
		return;
	}
	// Copies: a handler may change the limits.
	const std::map<int, int> limits = team(teamIndex).ticketLimits;
	for (const auto& [id, value] : limits) {
		const float limit = static_cast<float>(value);
		if (before > limit && after <= limit) {
			m_events("TicketLimitReached", {ScriptValue::integer(teamIndex), ScriptValue::integer(-id)});
		} else if (before <= limit && after > limit) {
			m_events("TicketLimitReached", {ScriptValue::integer(teamIndex), ScriptValue::integer(id)});
		}
	}
}

void GameLogic::setTickets(int teamIndex, float tickets)
{
	Team& t = team(teamIndex);
	const float before = std::floor(t.tickets);
	t.tickets = std::max(tickets, 0.0f);
	checkLimits(teamIndex, before, std::floor(t.tickets));
}

void GameLogic::tick(float seconds)
{
	if (m_ended) {
		return;
	}
	elapsed += seconds;
	for (int t = 1; t <= 2 && !m_ended; ++t) {
		const float change = team(t).ticketChangePerSecond;
		if (change != 0) {
			setTickets(t, team(t).tickets + change * seconds);
		}
	}
	if (!m_ended && timeLimit > 0 && elapsed >= timeLimit && !m_timeLimitRaised) {
		m_timeLimitRaised = true;
		if (m_events) {
			m_events("TimeLimitReached", {ScriptValue::integer(0)});
		}
	}
}

void GameLogic::endGame(int winner, int victoryType)
{
	if (m_ended) {
		return;
	}
	m_ended = true;
	m_winner = winner;
	m_victoryType = victoryType;
	if (onEndGame) {
		onEndGame(winner, victoryType);
	}
}

ScriptValue GameLogic::param(const std::string& name, int teamIndex, int id) const
{
	const Team& t = team(teamIndex);
	if (name == "tickets") {
		return ScriptValue::integer(static_cast<int64_t>(std::floor(t.tickets)));
	}
	if (name == "ticketState") {
		return ScriptValue::integer(t.ticketState);
	}
	if (name == "startTickets") {
		const auto it = t.defaultTickets.find(mapSize);
		return ScriptValue::integer(it != t.defaultTickets.end() ? it->second : 0);
	}
	if (name == "ticketChangePerSecond") {
		return ScriptValue::number(t.ticketChangePerSecond);
	}
	if (name == "ticketLimit") {
		const auto it = t.ticketLimits.find(id);
		return ScriptValue::integer(it != t.ticketLimits.end() ? it->second : 0);
	}
	if (name == "defaultTicketLossPerMin") {
		return ScriptValue::number(t.ticketLossPerMin);
	}
	if (name == "defaultTicketLossAtEndPerMin") {
		return ScriptValue::number(ticketLossAtEndPerMin);
	}
	if (name == "teamName") {
		return ScriptValue::string(t.name);
	}
	if (name == "winner") {
		return ScriptValue::integer(m_winner);
	}
	if (name == "victoryType") {
		return ScriptValue::integer(m_victoryType);
	}
	const auto it = limits.find(name);
	return it != limits.end() ? it->second : ScriptValue::none();
}

void GameLogic::setParam(const std::string& name, int teamIndex, const ScriptValue& value, int id)
{
	Team& t = team(teamIndex);
	if (name == "tickets") {
		setTickets(teamIndex, static_cast<float>(value.asNumber()));
	} else if (name == "ticketState") {
		t.ticketState = static_cast<int>(value.asInt());
	} else if (name == "ticketChangePerSecond") {
		t.ticketChangePerSecond = static_cast<float>(value.asNumber());
	} else if (name == "ticketLimit") {
		// bf2.GameLogic.setTicketLimit(team, id, value) passes the limit's
		// id where the other parameters have their value, and the value last.
		t.ticketLimits[static_cast<int>(value.asInt())] = id;
	} else {
		limits[name] = value;
	}
}

void registerGameLogicConsoleMethods(Console& console, GameLogic& logic)
{
	const uint32_t access = ConsoleAccess::Anyone;
	GameLogic* l = &logic;
	bindMethod(console, "GameLogic.setTeamName", access, [l](int team, std::string name) { l->team(team).name = name; });
	bindMethod(console, "GameLogic.setBeforeSpawnCamera", access, [l](std::string position, std::string rotation) {
		GameLogic::BeforeSpawnCamera& c = l->beforeSpawnCamera;
		c.set = std::sscanf(position.c_str(), "%f/%f/%f", &c.position[0], &c.position[1], &c.position[2]) == 3;
		std::sscanf(rotation.c_str(), "%f/%f/%f", &c.rotation[0], &c.rotation[1], &c.rotation[2]);
	});
	bindMethod(console, "GameLogic.setTeamLanguage", access,
		[l](int team, std::string language) { l->team(team).language = language; });
	bindMethod(console, "GameLogic.setTeamFlag", access, [l](int team, std::string flag) { l->team(team).flag = flag; });
	bindMethod(console, "GameLogic.setKit", access,
		[l](int team, int index, std::string kit, std::string soldier, std::string heavy) {
			auto& kits = l->team(team).kits;
			if (index >= 0 && index < 16) {
				if (kits.size() <= static_cast<size_t>(index)) {
					kits.resize(static_cast<size_t>(index) + 1);
				}
				kits[static_cast<size_t>(index)] = {kit, soldier, heavy};
			}
		});
	bindMethod(console, "GameLogic.setDefaultNumberOfTickets", access,
		[l](int team, int tickets) { l->team(team).defaultTickets[l->mapSize] = tickets; });
	bindMethod(console, "GameLogic.getDefaultNumberOfTickets", access, [l](int team) {
		const auto& t = l->team(team).defaultTickets;
		const auto it = t.find(l->mapSize);
		return it != t.end() ? it->second : 0;
	});
	bindMethod(console, "GameLogic.setDefaultNumberOfTicketsEx", access,
		[l](int mapSize, int team, int tickets) { l->team(team).defaultTickets[mapSize] = tickets; });
	bindMethod(console, "GameLogic.setNumberOfTickets", access,
		[l](int team, int tickets) { l->setTickets(team, static_cast<float>(tickets)); });
	bindMethod(console, "GameLogic.getNumberOfTickets", access,
		[l](int team) { return static_cast<int>(std::floor(l->team(team).tickets)); });
	bindMethod(console, "GameLogic.setTicketLossAtEndPerMin", access, [l](float loss) { l->ticketLossAtEndPerMin = loss; });
	bindMethod(console, "GameLogic.getTicketLossAtEndPerMin", access, [l]() { return l->ticketLossAtEndPerMin; });
	bindMethod(console, "GameLogic.setTicketLossPerMin", access,
		[l](int team, float loss) { l->team(team).ticketLossPerMin = loss; });
	bindMethod(console, "GameLogic.getTicketLossPerMin", access, [l](int team) { return l->team(team).ticketLossPerMin; });
}

} // namespace dice::hfe::game
