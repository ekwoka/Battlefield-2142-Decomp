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
if(WIN32)
	# python313.dll is copied beside bf2142.exe, where the interpreter finds no
	# Lib\; point it at the install the headers came from (C:/Python313/include).
	list(GET Python3_INCLUDE_DIRS 0 python_include)
	get_filename_component(python_home "${python_include}" DIRECTORY)
	target_compile_definitions(Scripting PRIVATE BF2142_PYTHON_HOME="${python_home}")
	if(NOT MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
		# Without MS_WIN64, pyconfig.h may assume 32-bit sizes under MinGW; empty matches its own define.
		target_compile_definitions(Scripting PRIVATE MS_WIN64=)
	endif()
endif()
