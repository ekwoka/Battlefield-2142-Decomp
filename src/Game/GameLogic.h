// The game logic of a round: teams, tickets, kits and the end of the round
// (BF2142.exe's GameLogic, the "gameLogic" console object and the sgl_*
// functions of the scripts' host module).
//
// Level scripts set it up (Levels/<name>/Init.con):
//   gameLogic.setTeamName 1 "Pac"
//   gameLogic.setKit 1 0 "Pac_Recon" "pac_light_soldier" "pac_heavy_soldier"
//   gameLogic.setDefaultNumberOfTicketsEx 32 1 220   (map size, team, tickets)
//   gameLogic.setTicketLossPerMin 1 6
// and the game mode's Python script runs the round through the parameters
// ("tickets", "ticketChangePerSecond", "ticketLimit"...). Each tick the
// tickets change by ticketChangePerSecond; crossing a ticket limit raises
// TicketLimitReached(team, -id) going down and (team, id) going up, and the
// time limit raises TimeLimitReached.
#pragma once

#include "ScriptGame.h"

#include <map>
#include <string>
#include <vector>

namespace dice::hfe {
class Console;
}

namespace dice::hfe::game {

struct Kit {
	std::string name;           // "Pac_Assault"
	std::string soldier;        // "pac_light_soldier"
	std::string heavySoldier;
};

struct Team {
	std::string name;
	std::string language;
	std::string flag;
	std::map<int, int> defaultTickets; // by map size
	float ticketLossPerMin = 0;
	std::vector<Kit> kits;              // by kit index

	// The round's state.
	float tickets = 0;
	float ticketChangePerSecond = 0;
	int ticketState = 0;
	std::map<int, int> ticketLimits; // id -> tickets
};

class GameLogic {
public:
	GameLogic();

	Team& team(int index); // 1 or 2 (0 for neutral)
	const Team& team(int index) const;
	int mapSize = 32;
	float ticketLossAtEndPerMin = 0;
	float timeLimit = 0; // seconds; 0: none
	float elapsed = 0;

	bool ended() const { return m_ended; }
	int winner() const { return m_winner; }
	int victoryType() const { return m_victoryType; }

	void setEventSink(ScriptEventSink sink) { m_events = std::move(sink); }
	// Called when the round ends (sgl_endGame or no tickets left).
	std::function<void(int winner, int victoryType)> onEndGame;

	void startRound();
	void tick(float seconds);
	void setTickets(int team, float tickets);
	void endGame(int winner, int victoryType);

	// sgl_getParam / sgl_setParam.
	ScriptValue param(const std::string& name, int team, int id) const;
	void setParam(const std::string& name, int team, const ScriptValue& value, int id);

	// healScoreLimit and the like, kept for the scripts.
	std::map<std::string, ScriptValue> limits;

private:
	void checkLimits(int team, float before, float after);

	Team m_teams[3];
	ScriptEventSink m_events;
	bool m_ended = false;
	int m_winner = 0;
	int m_victoryType = 0;
	bool m_timeLimitRaised = false;
};

// gameLogic.* console methods.
void registerGameLogicConsoleMethods(Console& console, GameLogic& logic);

} // namespace dice::hfe::game
