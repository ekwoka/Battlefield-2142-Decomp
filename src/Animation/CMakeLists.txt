add_library(Animation STATIC
	AnimationFiles.cpp
	AnimationConsole.cpp
	AnimationSystem.cpp
	FirstPerson.cpp
	Pose.cpp
	Ik.cpp
	RagDoll.cpp
)
target_include_directories(Animation PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(Animation PUBLIC Console)
