// A demo's playback controls and its camera, as BF2142.exe's demo reader
// keeps them (the client's demo interface, vtable 0x00928fa8, through
// QueryInterface 0x1d4c2; the reader built by 0x005177b0, its camera at
// +0xf0, 0xe0 bytes, constructor 0x005f93d0). No window code: shared with
// the tests.
//
//   demo.toggleDemoPauseOrPlay (0x00424b8f -> slot 0x5c, 0x004b32d0): the
//     game speed 0 or 100 (client slot 0xe8, 0x004b3b90 -> the time
//     manager's scale, 0x006da7a0); refused at the demo's end (+0xf4).
//   Game.simulationRate <pct> (0x00424029): the speed (slots 0xec / 0xe8),
//     set only while a demo is read; the DemoRecorder rose's 50/25/10/150/300.
//   demo.restartDemo (slot 0x48, 0x004b3190): the demo from its start and
//     a new camera (0x005178d0 -> 0x005177b0); the speed kept.
//   demo.shutdownDemo (0x0042486e -> 0x00404750 -> 0x00403c50): the speed
//     back to 100 and the game left.
//   demo.adjustDemoFov <deg> (0x00424947 -> 0x004030b0): the reader's
//     options +0x68, demo.renderfieldofview in degrees, as given (no step,
//     no clamp); "This setting can only be set when a demo is active" without
//     one. Each frame (0x00434610) the view's fov is pi / 180 x fov x
//     renderheight / renderwidth (400 x 300 by default, 0x00439f80: 90 ->
//     1.178 rad) with the aspect 0.75.
//   demo.togglePlayerDemo <bool> (slot 0x4c, 0x004b3200 -> 0x005fa440 /
//     0x005fa470): the next (1) or previous (0) player followed;
//   demo.toggleCameraDemo (slot 0x50 -> 0x005fa430 -> 0x005fa130): follow
//     <-> free.
//
// The camera starts free (+0x10 = 0; its FreeCamera "MultiPlayerFreeCamera"
// at (0, 75, 0), 0x005f9f50). A step (0x005f9e30 + 0x005f8cc0) follows (+0x10
// = 1): from ((the current id, or 0) + step) & 0xff it tries 256 ids, stepping
// and wrapping, for the first player alive with a control object (any
// team); without one it goes free. toggleCamera steps by 0 from free (the
// current player first), and from follow goes free. The follow view
// (0x005f8f40): the followed control object's matrix M; R the rotation of
// (yaw +0x58, pitch +0x5c) degrees; target = M's position + M.up x height
// (+0x9c) + M.right x side (+0xa0); eye = target - R's forward x distance
// (+0x98, 2), or that offset turned by M when target-aligned (+0x88);
// lookAt(eye, target, up).
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

namespace dice::hfe::host {

struct DemoCamera {
	enum class Mode { Free = 0, Follow = 1 };
	Mode mode = Mode::Free;
	int followed = -1;      // +0x8: the player id, -1 none
	float yaw = 0, pitch = 0; // +0x58 / +0x5c, degrees
	float distance = 2;     // +0x98
	float height = 0;       // +0x9c
	float side = 0;         // +0xa0
	bool targetAligned = false; // +0x88

	// 0x005f9e30: the next eligible player from `followed` by `step`.
	bool step(int dir, const std::function<bool(int id)>& eligible)
	{
		int id = ((followed < 0 ? 0 : followed) + dir) & 0xff;
		for (int tries = 0; tries < 256; ++tries) {
			if (eligible(id)) {
				followed = id;
				mode = Mode::Follow;
				return true;
			}
			id = (id + (dir == 0 ? 1 : dir)) & 0xff;
		}
		mode = Mode::Free;
		return false;
	}
	// 0x005fa130.
	void toggle(const std::function<bool(int id)>& eligible)
	{
		if (mode == Mode::Follow) {
			mode = Mode::Free;
		} else {
			step(0, eligible);
		}
	}

	struct Look {
		float eye[3] {};
		float yaw = 0, pitch = 0; // radians, yaw 0 along +z, pitch up positive
	};
	// The follow view about an object at `position` turned by `objectYaw`
	// (degrees): its right and up the yawed axes.
	Look follow(const float position[3], float objectYawDegrees) const
	{
		constexpr float kRad = 3.14159265f / 180.0f;
		const float oy = objectYawDegrees * kRad;
		const float right[3] = {std::cos(oy), 0, -std::sin(oy)};
		const float target[3] = {position[0] + right[0] * side, position[1] + height, position[2] + right[2] * side};
		float y = yaw * kRad, p = pitch * kRad;
		if (targetAligned) {
			y += oy;
		}
		const float forward[3] = {std::sin(y) * std::cos(p), std::sin(p), std::cos(y) * std::cos(p)};
		Look look;
		for (int i = 0; i < 3; ++i) {
			look.eye[i] = target[i] - forward[i] * distance;
		}
		const float d[3] = {target[0] - look.eye[0], target[1] - look.eye[1], target[2] - look.eye[2]};
		look.yaw = std::atan2(d[0], d[2]);
		look.pitch = std::atan2(d[1], std::sqrt(d[0] * d[0] + d[2] * d[2]));
		return look;
	}
};

struct DemoPlayback {
	int speed = 100;       // percent (the time manager's scale)
	bool ended = false;    // +0xf4: the demo read to its end
	float fov = 90;        // degrees (demo.renderfieldofview)
	float renderWidth = 400, renderHeight = 300;
	DemoCamera camera;

	// toggleDemoPauseOrPlay: false when refused.
	bool togglePause()
	{
		if (speed == 0) {
			if (ended) {
				return false;
			}
			speed = 100;
		} else {
			speed = 0;
		}
		return true;
	}
	void restart()
	{
		camera = {};
		ended = false;
	}
	float fovY() const { return 3.14159265f / 180.0f * fov * renderHeight / renderWidth; }
	// Ticks due for `realTicks` more of real time at the speed, the
	// fraction kept in `carry`.
	int advance(int realTicks, double& carry) const
	{
		carry += realTicks * (speed / 100.0);
		const int whole = static_cast<int>(std::floor(carry));
		carry -= whole;
		return whole;
	}
};

} // namespace dice::hfe::host
