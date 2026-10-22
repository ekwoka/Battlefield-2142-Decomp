# The game's Python scripting, on an embedded Python 3 (optional: without
# Python the game runs without its game-mode scripts).
if(NOT TARGET Python3::Python)
	return()
endif()
set(embedded ${CMAKE_CURRENT_BINARY_DIR}/EmbeddedScripts.h)
add_custom_command(OUTPUT ${embedded}
	COMMAND ${CMAKE_COMMAND} -DOUT=${embedded} -DPY2COMPAT=${CMAKE_CURRENT_SOURCE_DIR}/py2compat.py
		-DHOSTOBJECTS=${CMAKE_CURRENT_SOURCE_DIR}/hostobjects.py -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
	DEPENDS py2compat.py hostobjects.py embed.cmake)
add_library(Scripting STATIC ScriptHost.cpp ${embedded})
target_include_directories(Scripting PUBLIC ${PROJECT_SOURCE_DIR}/src PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(Scripting PUBLIC Game PRIVATE Python3::Python)
target_compile_definitions(Scripting PUBLIC BF2142_HAS_PYTHON=1)
