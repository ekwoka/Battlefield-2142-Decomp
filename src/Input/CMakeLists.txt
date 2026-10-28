add_library(Input STATIC
	ControlMap.cpp
	GameControllers.cpp
)
target_include_directories(Input PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Input PUBLIC Console)

# SDL3's joysticks and gamepads (the window's side), when the build has
# SDL3 (as the renderer).
if(NOT CMAKE_CROSSCOMPILING)
	find_package(SDL3 CONFIG QUIET)
endif()
if(SDL3_FOUND)
	add_library(InputSdl STATIC SdlGameControllers.cpp)
	target_link_libraries(InputSdl PUBLIC Input SDL3::SDL3)
endif()
