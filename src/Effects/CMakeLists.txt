add_library(Effects STATIC
	Effects.cpp
)
target_include_directories(Effects PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Effects PUBLIC World Physics Scene)
