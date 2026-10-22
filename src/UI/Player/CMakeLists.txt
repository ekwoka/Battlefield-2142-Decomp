add_library(SwfPlayer STATIC
	ClipMethods.cpp
	DisplayList.cpp
	Draw.cpp
	Natives.cpp
	NativeTable.cpp
	Player.cpp
	Stage.cpp
	Stencil.cpp
)
target_include_directories(SwfPlayer PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(SwfPlayer PUBLIC SwfCharacters UI Avm1 Hud)
