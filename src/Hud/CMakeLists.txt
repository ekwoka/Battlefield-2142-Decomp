add_library(Hud STATIC
	Hud.cpp
	HudFont.cpp
)
target_include_directories(Hud PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Hud PUBLIC Console)
