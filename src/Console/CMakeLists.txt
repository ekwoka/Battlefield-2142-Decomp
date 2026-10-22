add_library(Console STATIC
	Console.cpp
)
target_include_directories(Console PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Console PUBLIC IO)
