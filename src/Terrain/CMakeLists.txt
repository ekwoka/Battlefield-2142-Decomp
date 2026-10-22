add_library(Terrain STATIC
	Heightmap.cpp
	TerrainData.cpp
	Undergrowth.cpp
)
target_include_directories(Terrain PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Terrain PUBLIC Console IO)
