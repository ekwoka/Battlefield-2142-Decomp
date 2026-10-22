add_library(AI STATIC
	Behaviour.cpp
	PathFollower.cpp
	NavMesh.cpp
	Strategy.cpp
)
target_include_directories(AI PUBLIC ${PROJECT_SOURCE_DIR}/src)
