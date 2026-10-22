// Parses every skeleton and animation of the game. Needs $BF2142_GAME_DIR.
#include "Animation/AnimationFiles.h"
#include "Animation/Pose.h"
#include "IO/File/ZipArchive.h"
#include "TestMain.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
namespace anim = dice::hfe::anim;

TEST(AnimationFiles_ParseAllGameFiles)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	int skeletons = 0, animations = 0, failures = 0;
	float worstQuaternion = 0;
	for (const auto& entry : fs::recursive_directory_iterator(fs::path(gameDir) / "mods/bf2142")) {
		if (entry.path().extension() != ".zip") {
			continue;
		}
		auto zip = dice::hfe::io::ZipArchive::open(entry.path().string());
		if (!zip) {
			continue;
		}
		for (const std::string& name : zip->listRecursive("")) {
			const bool isSkeleton = name.size() > 4 && name.compare(name.size() - 4, 4, ".ske") == 0;
			const bool isAnimation = name.size() > 4 && name.compare(name.size() - 4, 4, ".baf") == 0;
			if (!isSkeleton && !isAnimation) {
				continue;
			}
			std::vector<uint8_t> data;
			zip->read(name, data);
			std::string error;
			if (isSkeleton) {
				if (anim::parseSkeleton(data, &error)) {
					++skeletons;
				} else {
					std::fprintf(stderr, "  %s: %s\n", name.c_str(), error.c_str());
					++failures;
				}
				continue;
			}
			const auto animation = anim::parseAnimation(data, &error);
			if (!animation) {
				std::fprintf(stderr, "  %s: %s\n", name.c_str(), error.c_str());
				++failures;
				continue;
			}
			++animations;
			// Rotations decode to unit quaternions (a few files leave unused
			// bones all zero).
			for (const auto& bone : animation->bones) {
				for (const auto& q : bone.rotations) {
					const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
					if (length > 0.01f) {
						worstQuaternion = std::fmax(worstQuaternion, std::fabs(length - 1));
					}
				}
			}
		}
	}
	std::fprintf(stderr, "  %d skeletons, %d animations parsed; worst quaternion length error %.4f\n", skeletons,
		animations, worstQuaternion);
	CHECK(failures == 0);
	CHECK(skeletons >= 96);
	CHECK(animations >= 5585);
	CHECK(worstQuaternion < 0.01f);
}

// The quaternion convention: a standing animation must keep the head above
// the feet and upright. (The setup pose looks right with either convention,
// because each rig matrix undoes it.)
TEST(AnimationFiles_StandingPoseIsUpright)
{
	const char* gameDir = std::getenv("BF2142_GAME_DIR");
	if (!gameDir) {
		return;
	}
	auto zip = dice::hfe::io::ZipArchive::open(
		(fs::path(gameDir) / "mods/bf2142/Objects/Soldiers_server.zip").string());
	CHECK(zip != nullptr);
	std::vector<uint8_t> skeletonData, animationData;
	CHECK(zip->read("Common/Animations/3p_setup.ske", skeletonData));
	CHECK(zip->read("Common/Animations/3P/3p_stand.baf", animationData));
	const auto skeleton = anim::parseSkeleton(skeletonData);
	const auto animation = anim::parseAnimation(animationData);
	CHECK(skeleton && animation);
	const auto pose = anim::evaluatePose(*skeleton, &*animation, 0);
	const int head = skeleton->find("head");
	const int foot = skeleton->find("left_foot");
	CHECK(head >= 0 && foot >= 0);
	// Row 3 of a bone's matrix is its position.
	const float headY = pose[static_cast<size_t>(head)][13];
	const float footY = pose[static_cast<size_t>(foot)][13];
	CHECK(headY - footY > 1.2f);
}
