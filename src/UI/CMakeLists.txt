add_library(UI STATIC
	Dfm.cpp
	Swf.cpp
)
target_include_directories(UI PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(UI PRIVATE zlibstatic)

add_subdirectory(Avm1)
add_subdirectory(Characters)
add_subdirectory(Player)
