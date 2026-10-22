add_library(Geometry STATIC
	GeometryTemplateManager.cpp
	MeshFile.cpp
	RoadFile.cpp
)
target_include_directories(Geometry PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Geometry PUBLIC Console)
