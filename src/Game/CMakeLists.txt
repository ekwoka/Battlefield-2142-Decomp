add_library(Game STATIC
	Bots.cpp
	GameWorld.cpp
	Commander.cpp
	SoldierControl.cpp
	Weapons.cpp
	Vehicle.cpp
	GameLogic.cpp
	ScriptBridge.cpp
	TickTimer.cpp
)
target_include_directories(Game PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Game PUBLIC World Physics Effects Animation Console AI)
