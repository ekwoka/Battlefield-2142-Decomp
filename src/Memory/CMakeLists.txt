add_library(Memory STATIC Memory.cpp ChunkPools.cpp)
target_include_directories(Memory PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Memory PRIVATE Platform)

if(BF2142_RETAIL_COMPAT)
	# Drop-in Memory.dll for the retail game.
	add_library(MemoryDll SHARED retail/MemoryDll.cpp retail/Memory.def)
	set_target_properties(MemoryDll PROPERTIES OUTPUT_NAME Memory PREFIX "")
	target_link_libraries(MemoryDll PRIVATE Memory)
	target_link_options(MemoryDll PRIVATE -static -static-libgcc -static-libstdc++)
endif()
