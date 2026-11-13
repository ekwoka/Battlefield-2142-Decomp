add_library(Game STATIC
	Bots.cpp
	BotsScript.cpp
	BotsSquad.cpp
	BotsLook.cpp
	BotsMines.cpp
	GameWorld.cpp
	MeshCollision.cpp
	Ladders.cpp
	Props.cpp
	Commander.cpp
	Titans.cpp
	Kits.cpp
	CombatAreas.cpp
	SoldierControl.cpp
	Weapons.cpp
	Vehicle.cpp
	VehicleParts.cpp
	GameLogic.cpp
	Radio.cpp
	ScriptBridge.cpp
	TickTimer.cpp
	Cameras.cpp
	Votes.cpp
	SquadRequests.cpp
	AmbientTriggers.cpp
	Ambience.cpp
	LockFeedback.cpp
	TemplateBehaviours.cpp
	SeatBehaviours.cpp
	TeamDamage.cpp
	Parachutes.cpp
	PartCollision.cpp
)
target_include_directories(Game PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Game PUBLIC World Physics Effects Animation Console AI)
