add_library(Stats STATIC
	Stats.cpp
)
target_include_directories(Stats PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Stats PUBLIC Console World)
