add_library(AI STATIC
	Ballistics.cpp
	Behaviour.cpp
	Heli.cpp
	PathFollower.cpp
	NavMesh.cpp
	Strategy.cpp
)
target_include_directories(AI PUBLIC ${PROJECT_SOURCE_DIR}/src)
