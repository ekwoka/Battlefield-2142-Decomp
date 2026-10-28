// The game as its scripts see it (ScriptGame over GameWorld and GameLogic).
//
// One local player, index 0. Player control objects (what "<<PCO>>"
// triggers look for) are the player's soldier and the vehicles. A soldier
// in a vehicle is that vehicle's child.
#pragma once

#include "GameLogic.h"
#include "GameWorld.h"
#include "ScriptGame.h"

#include <map>

namespace dice::hfe::game {

struct ServerSettings {
	int ticketRatio = 100;
	int teamRatioPercent = 100;
	int maxPlayers = 64;
	std::string gameMode = "gpm_cq";
	std::string mapName;
	int timeLimit = 0;
	int scoreLimit = 0;
	int maxRank = 0;
};

class ScriptBridge : public ScriptGame {
public:
	ScriptBridge(GameWorld& world, GameLogic& logic, ServerSettings settings, std::string modDirectory);

	double wallTime() const override { return m_logic.elapsed; }

	ScriptValue gameParam(const std::string& name, int team, int id) override;
	void setGameParam(const std::string& name, int team, const ScriptValue& value, int id) override;
	ScriptValue serverSetting(const std::string& name) override;
	std::string mapName() override { return m_settings.mapName; }
	std::string modDirectory() override { return m_modDirectory; }
	int worldSize() override { return 1024; }
	bool isAIGame() override { return false; }
	void endGame(int winner, int victoryType) override { m_logic.endGame(winner, victoryType); }
	void textMessage(int player, const std::string& text) override;
	void gameEvent(const std::string& kind, const ScriptArgs& args) override;

	std::vector<uint32_t> objectsOfType(const std::string& type) override;
	std::vector<uint32_t> objectsOfTemplate(const std::string& name) override;
	ScriptValue objectCall(uint32_t object, const std::string& method, const ScriptArgs& args) override;
	std::vector<uint32_t> triggerCandidates(const std::string& filter) override;
	bool objectPosition(uint32_t object, float position[3]) override;

	std::vector<int> players() override;
	bool playerValid(int index) override;
	ScriptValue playerParam(const std::string& name, int index, const ScriptArgs& extra) override;
	void setPlayerParam(const std::string& name, int index, const ScriptValue& value) override;
	ScriptValue playerScore(int index, const std::string& name) override;
	void setPlayerScore(int index, const std::string& name, const ScriptValue& value) override;
	int commander(int team) override { return m_world.commander(team); }
	void setCommanderEnabled(bool enabled) override { m_world.setCommanderEnabled(enabled); }
	std::vector<uint32_t> controlPointsInGroup(int group) override { return m_world.controlPointsInGroup(group); }
	int squadLeader(int team, int squad) override;
	std::vector<int> playersInSquad(int team, int squad) override;
	void setScoreEvents(bool enabled) override { m_scoreEvents = enabled; }
	bool giveFieldUnlock(int player) override;
	void endOfRoundData(const std::string& text) override;

	// Where PlayerScore goes (the scripts' handlers; main sets it).
	ScriptEventSink events;

	// Messages and game events the scripts sent, newest last (for the HUD).
	std::vector<std::string> messages;

private:
	ScriptValue controlPointParam(ControlPoint& point, const std::string& name, const ScriptArgs& args);
	void setControlPointParam(ControlPoint& point, const std::string& name, const ScriptArgs& args);
	uint32_t parentOf(uint32_t object);

	GameWorld& m_world;
	GameLogic& m_logic;
	ServerSettings m_settings;
	std::string m_modDirectory;
	std::map<int, std::map<std::string, ScriptValue>> m_playerParams; // set by scripts, by player
	std::map<int, std::map<std::string, ScriptValue>> m_scores;
	bool m_scoreEvents = false;
};

} // namespace dice::hfe::game
