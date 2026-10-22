add_library(Input STATIC
	ControlMap.cpp
)
target_include_directories(Input PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Input PUBLIC Console)
