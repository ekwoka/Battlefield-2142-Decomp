# The ActionScript 1/2 interpreter for the Flash menus (SWF 7 AVM1).
add_library(Avm1 STATIC
	Value.cpp
	Object.cpp
	Interpreter.cpp
	Execute.cpp
	Builtins.cpp
	BuiltinsArray.cpp
	BuiltinsString.cpp
	BuiltinsDate.cpp
	BuiltinsStubs.cpp
)
target_include_directories(Avm1 PUBLIC ${PROJECT_SOURCE_DIR}/src)
