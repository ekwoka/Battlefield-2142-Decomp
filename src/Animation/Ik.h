// The hands' IK (BF2142.exe's `ik.*`, the template component Ik 0xc4cc /
// interface 0xc4cd): in a seat whose tree holds an object with the
// component (a car's steering wheel, a stationary gun), the third-person
// skeleton's arms reach its grips. 0x005efc00 sets the two targets each
// frame (0x008b92b0 into the skeleton's +0x1c list), and the hierarchy pass
// (0x008b8880) solves each arm when it reaches it (0x008b6760: Maya's
// ik2Bsolver, 0x008c26f0 / 0x008c2160, twist 0). There is no blend: the
// arm's bones are overridden, and the hand takes the target whole.
#pragma once

#include "Pose.h"

#include <string>

namespace dice::hfe::anim {

struct IkTarget {
	// The bone B the solve runs at and the end bone that takes the target
	// (the left arm: joint19 then left_wrist1, B 0x12, end 0x13; the right:
	// right_ullna for both, 0x22). B - 3, B - 2 and B - 1 are the shoulder,
	// the elbow and the lower arm (the skeleton's order).
	int bone = -1, end = -1;
	Matrix target {};      // in the skeleton's space (rows the axes, row 3 the place)
	Position pole {0, 0, 0}; // in the same space; zero: no pole turn
};

// The three-part arm solve of 0x008b6760 on `world`, which must hold the
// pass's matrices up to bone - 1. Returns the solved arm's last bone (end).
void solveArm(const Skeleton& skeleton, const LocalPose& pose, std::vector<Matrix>& world, const IkTarget& target);

// The hierarchy pass with the IK (0x008b8880): the bones in order, each
// target solved where the walk reaches its bone, then on from its end.
std::vector<Matrix> worldMatricesWithIk(const Skeleton& skeleton, const LocalPose& pose, const std::vector<IkTarget>& targets);

// The arms of the soldiers' 3P skeleton (3p_setup.ske) by name, -1 when
// the skeleton lacks them: {bone, end}.
struct IkArms {
	int leftBone = -1, leftEnd = -1, rightBone = -1, rightEnd = -1;
};
IkArms ikArms(const Skeleton& skeleton);

} // namespace dice::hfe::anim
