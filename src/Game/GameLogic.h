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

	// The level's camera before the player's first spawn
	// (GameLogic.setBeforeSpawnCamera <position> <rotation>, 0x004278ef:
	// the levels' Init.con set one per map), degrees yaw/pitch/roll.
	struct BeforeSpawnCamera {
		bool set = false;
		float position[3] = {};
		float rotation[3] = {};
	};
	BeforeSpawnCamera beforeSpawnCamera;

	// The scripts' end-of-round text (gl_sendEndOfRoundData, 0x00568930:
	// bf2/stats/endofround.py's "\\key\\value..." list), which the server
	// sends its clients as data block 8 with the delay; the serial counts
	// the times it was set (a hosted game sends each once).
	void setEndOfRoundData(const std::string& text)
	{
		m_endOfRoundData = text;
		++m_endOfRoundSerial;
	}
	const std::string& endOfRoundData() const { return m_endOfRoundData; }
	int endOfRoundSerial() const { return m_endOfRoundSerial; }

private:
	void checkLimits(int team, float before, float after);

	Team m_teams[3];
	ScriptEventSink m_events;
	bool m_ended = false;
	int m_winner = 0;
	int m_victoryType = 0;
	bool m_timeLimitRaised = false;
	std::string m_endOfRoundData;
	int m_endOfRoundSerial = 0;
};

// gameLogic.* console methods.
void registerGameLogicConsoleMethods(Console& console, GameLogic& logic);

} // namespace dice::hfe::game
