add_library(World STATIC
	Object.cpp
	ObjectTemplateManager.cpp
	TemplateFiles.cpp
	WorldConsole.cpp
)
target_include_directories(World PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(World PUBLIC Console)
