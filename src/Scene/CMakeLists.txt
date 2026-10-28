add_library(Scene STATIC
	LightSettings.cpp
	PostProductionSettings.cpp
)
target_include_directories(Scene PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Scene PUBLIC Console)
