add_library(Physics STATIC
	CollisionMeshFile.cpp
	CollisionTemplateManager.cpp
	CollisionWorld.cpp
	MaterialManager.cpp
	RigidBody.cpp
	SoldierPhysics.cpp
)
target_include_directories(Physics PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Physics PUBLIC Console IO)
