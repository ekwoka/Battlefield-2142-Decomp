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
//
// Camera files (.bf2142cam, net::CameraKeyframe): the demo object's camera
// mode (the reader's +8, demo.cameramode: user 0, record 1, playback 2,
// 0x0043a2e0) opens one beside the demo (demo.camerafile, +0x28):
//   playback with cameramode 1 (0x00517490 -> 0x005171f0, camera +0x70)
//     records the demo camera while the demo plays, then runs
//     Settings/demo-recordcam.con; with cameramode 2 (-> 0x00516830,
//     camera +0x14 / +0x6c) the camera plays the file, then runs
//     Settings/demo-playcam.con (console slot 0x24, no arguments; no
//     shipped mod has either file).
//   record (0x005172f0, demo.recordDemo 0x00432cc0 with a local player):
//     the demo opened for writing (0x005170f0), then a camera of its own
//     recording <name>-def.bf2142cam (0x005171f0) and
//     Settings/demo-recordcam.con.
// DemoCameraRecorder is the writer (0x005f9630), DemoCameraTrack the
// player (0x005f9cd0, 0x005fa210, 0x005fa4a0).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Net/Demo.h"

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

// demo.recordDemo's camera file (0x00432cc0): the name as given ('\\' as
// '/', 0x0065bce0) with "-def.bf2142cam", under Demos/ when it has no
// extension (as net::demoPath puts the demo there).
inline std::string demoCameraPath(std::string name)
{
	std::replace(name.begin(), name.end(), '\\', '/');
	const size_t slash = name.rfind('/');
	if (name.find('.', slash == std::string::npos ? 0 : slash) == std::string::npos) {
		name = "Demos/" + name;
	}
	return name + "-def.bf2142cam";
}

// The camera's matrix as camera files keep it: rows right, up, forward
// (row-major, as the engine's matrices), and its quaternion (x, y, z, w).
namespace demo_camera {

// The rows of a camera turned by yaw (0 along +z) and pitch (up
// positive), radians.
inline void rowsFromYawPitch(float yaw, float pitch, float m[9])
{
	const float sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
	const float rows[9] = {cy, 0, -sy, -sp * sy, cp, -sp * cy, sy * cp, sp, cy * cp};
	std::copy(rows, rows + 9, m);
}

// 0x006ac740: the quaternion of a rotation matrix (m[r * 3 + c]), then
// normalised (0x005fdba0).
inline void quaternionFromRows(const float m[9], float q[4])
{
	const float trace = m[0] + m[4] + m[8];
	if (trace > 0) {
		float s = std::sqrt(trace + 1);
		q[3] = s * 0.5f;
		s = 0.5f / s;
		q[0] = (m[7] - m[5]) * s;
		q[1] = (m[2] - m[6]) * s;
		q[2] = (m[3] - m[1]) * s;
	} else {
		static constexpr int kNext[3] = {1, 2, 0}; // DAT_00a06578
		int i = m[0] < m[4] ? 1 : 0;
		if (m[i * 4] < m[8]) {
			i = 2;
		}
		const int j = kNext[i], k = kNext[j];
		float s = std::sqrt(m[i * 4] - (m[k * 4] + m[j * 4]) + 1);
		q[i] = s * 0.5f;
		if (std::fabs(s) > 2.3841858e-07f) {
			s = 0.5f / s; // (else s is used as it is, as the original does)
		}
		q[3] = (m[k * 3 + j] - m[j * 3 + k]) * s;
		q[j] = (m[j * 3 + i] + m[i * 3 + j]) * s;
		q[k] = (m[k * 3 + i] + m[i * 3 + k]) * s;
	}
	const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	if (n > 0) {
		for (int c = 0; c < 4; ++c) {
			q[c] /= n;
		}
	}
}

// 0x006ac870: the rotation matrix of a quaternion.
inline void rowsFromQuaternion(const float q[4], float m[9])
{
	const float n = 2 / (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	const float x = q[0], y = q[1], z = q[2], w = q[3];
	m[0] = 1 - n * (y * y + z * z);
	m[1] = n * (x * y - z * w);
	m[2] = n * (x * z + y * w);
	m[3] = n * (x * y + z * w);
	m[4] = 1 - n * (x * x + z * z);
	m[5] = n * (y * z - x * w);
	m[6] = n * (x * z - y * w);
	m[7] = n * (y * z + x * w);
	m[8] = 1 - n * (x * x + y * y);
}

// 0x006ac980: the spherical blend from a to b by t, the short way round
// (a straight blend within 1e-6 of a half turn's ends).
inline void slerp(const float a[4], const float b[4], float t, float out[4])
{
	float cosine = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
	const float sign = cosine < 0 ? -1.0f : 1.0f;
	cosine *= sign;
	float wa = 1 - t, wb = t;
	if (1 - cosine > 1e-6f) {
		const float angle = std::acos(std::clamp(cosine, -1.0f, 1.0f));
		const float inv = 1 / std::sin(angle);
		wa = std::sin((1 - t) * angle) * inv;
		wb = std::sin(angle * t) * inv;
	}
	wb *= sign;
	for (int c = 0; c < 4; ++c) {
		out[c] = wa * a[c] + wb * b[c];
	}
}

// A forward row's yaw (0 along +z) and pitch (up positive), radians.
inline void yawPitchOfRows(const float m[9], float& yaw, float& pitch)
{
	yaw = std::atan2(m[6], m[8]);
	pitch = std::atan2(m[7], std::sqrt(m[6] * m[6] + m[8] * m[8]));
}

// The camera object's four-frame ring (+0x68, 4 x 0x44 bytes, +0x64 the
// count; constructor 0x005f93d0): every frame free at the origin, no
// player followed.
inline std::array<net::CameraKeyframe, 4> emptyRing()
{
	std::array<net::CameraKeyframe, 4> ring;
	for (net::CameraKeyframe& f : ring) {
		f.followed = 0xffffffffu;
	}
	return ring;
}

} // namespace demo_camera

// 0x005f9630, once a frame with the ticks gone by: the frame now (the
// local player's view on a server with one, else the demo camera's own
// state) goes into the ring, and the file gets that frame smoothed over
// the ring's four, its position their mean and its rotation the blend
// ((q0 . q1) . q2) . q3 at one half each; once for every tick gone by,
// stamped now, now + 1, ... The ring starts at the origin, so the first
// three frames are pulled towards it, as the original's are.
class DemoCameraRecorder {
public:
	void record(int ticks, uint32_t now, const net::CameraKeyframe& current)
	{
		if (ticks <= 0) {
			return;
		}
		for (size_t i = 0; i + 1 < m_ring.size(); ++i) {
			m_ring[i] = m_ring[i + 1];
		}
		m_ring.back() = current;
		net::CameraKeyframe frame = current;
		for (int c = 0; c < 3; ++c) {
			float sum = 0;
			for (const net::CameraKeyframe& f : m_ring) {
				sum += f.position[c];
			}
			frame.position[c] = sum * (1.0f / static_cast<float>(m_ring.size()));
		}
		float a[4], b[4];
		demo_camera::slerp(m_ring[0].rotation, m_ring[1].rotation, 0.5f, a);
		demo_camera::slerp(a, m_ring[2].rotation, 0.5f, b);
		demo_camera::slerp(b, m_ring[3].rotation, 0.5f, frame.rotation);
		for (int i = 0; i < ticks; ++i) {
			frame.tick = now + static_cast<uint32_t>(i);
			m_frames.push_back(frame);
		}
	}
	const std::vector<net::CameraKeyframe>& frames() const { return m_frames; }
	void restart() { *this = {}; }

private:
	std::array<net::CameraKeyframe, 4> m_ring = demo_camera::emptyRing();
	std::vector<net::CameraKeyframe> m_frames;
};

// A camera file played (the camera's +0x14; 0x005fa4a0 takes 0x005fa210
// whatever the camera's own mode).
class DemoCameraTrack {
public:
	DemoCameraTrack() = default;
	// `sync` (demo.synccameraanddemo, the options' +0x74, on by default):
	// the frames' ticks are the game's; off, the first frame plays when
	// the camera starts.
	explicit DemoCameraTrack(std::vector<net::CameraKeyframe> file, bool sync = true)
		: m_file(std::move(file)), m_sync(sync)
	{
	}

	// 0x005f9cd0: the ring kept full from the file. Frames before `now`
	// go (four at most a call), then up to four are read, those before
	// now read over; true when all four are there. Once the file has no
	// more frames the track is over (+0x15) and the ring isn't played
	// out: 0x00432ee0 then ends the demo.
	bool fill(uint32_t now)
	{
		if (m_next >= m_file.size()) {
			m_ended = true;
			return false;
		}
		for (int dropped = 0; m_count > 0 && dropped < kRing && m_ring[0].tick < now; ++dropped) {
			for (int i = 0; i + 1 < m_count; ++i) {
				m_ring[static_cast<size_t>(i)] = m_ring[static_cast<size_t>(i + 1)];
			}
			--m_count;
		}
		while (m_count < kRing && m_next < m_file.size()) {
			net::CameraKeyframe& slot = m_ring[static_cast<size_t>(m_count)];
			read(slot, m_file[m_next++]);
			if (!m_started && !m_sync) {
				m_offset = now - slot.tick; // +0xd4
			}
			m_started = true; // +0xd1
			slot.tick += m_offset;
			if (slot.tick >= now) {
				++m_count;
			}
		}
		return m_count == kRing;
	}
	bool ended() const { return m_ended; }

	// What the camera shows (0x005fa210 at the frame's fraction of a tick,
	// the demo's +0xf8): a free frame's rotation and position, blended
	// from the ring's first frame to its second; a follow frame's player
	// with the follow view's angles and distances blended (onto `camera`,
	// its followed player and targetAligned the first frame's); attached
	// (+0xbc), the follow view's eye with the file's rotation.
	struct Pose {
		bool follow = false;
		bool attached = false;
		float rotation[4] = {0, 0, 0, 1};
		float position[3] = {0, 0, 0};
	};
	bool update(uint32_t now, float t, DemoCamera& camera, Pose& pose)
	{
		if (!fill(now)) {
			return false; // the camera keeps its last matrix
		}
		const net::CameraKeyframe& a = m_ring[0];
		const net::CameraKeyframe& b = m_ring[1];
		pose = {};
		if (a.mode == 0) {
			demo_camera::slerp(a.rotation, b.rotation, t, pose.rotation);
			for (int c = 0; c < 3; ++c) {
				pose.position[c] = a.position[c] * (1 - t) + b.position[c] * t; // 0x005f9370
			}
			return true;
		}
		const auto blend = [t](float x, float y) { return t * y + (1 - t) * x; };
		camera.yaw = blend(a.follow[0], b.follow[0]);
		camera.pitch = blend(a.follow[1], b.follow[1]);
		camera.distance = blend(a.follow[2], b.follow[2]);
		camera.height = blend(a.follow[3], b.follow[3]);
		camera.side = blend(a.follow[4], b.follow[4]);
		camera.followed = a.followed == 0xffffffffu ? -1 : static_cast<int>(a.followed);
		camera.targetAligned = a.followFlag != 0;
		pose.follow = true;
		pose.attached = a.attached;
		if (a.attached) {
			demo_camera::slerp(a.rotation, b.rotation, t, pose.rotation);
		}
		return true;
	}
	void restart() { *this = DemoCameraTrack(std::move(m_file), m_sync); }

private:
	static constexpr int kRing = 4;
	// 0x005f91f0 reads into the ring's slot: a free frame (mode 0,
	// attached or override) over its rotation and position only, a follow
	// frame over its player and follow values only, so an attached
	// frame's follow values are what the slot held (played a frame a
	// tick, the frame read before it; after a jump, an older one).
	static void read(net::CameraKeyframe& slot, const net::CameraKeyframe& f)
	{
		slot.tick = f.tick;
		slot.mode = f.mode;
		slot.attached = f.attached;
		slot.override = f.override;
		if (f.free()) {
			std::copy(f.rotation, f.rotation + 4, slot.rotation);
			std::copy(f.position, f.position + 3, slot.position);
		} else {
			slot.followed = f.followed;
			slot.followFlag = f.followFlag;
			std::copy(f.follow, f.follow + 5, slot.follow);
		}
	}

	std::vector<net::CameraKeyframe> m_file;
	size_t m_next = 0;
	std::array<net::CameraKeyframe, 4> m_ring = demo_camera::emptyRing();
	int m_count = 0;      // +0x60
	bool m_sync = true;
	bool m_started = false;
	uint32_t m_offset = 0;
	bool m_ended = false;
};

struct DemoPlayback {
	int speed = 100;       // percent (the time manager's scale)
	bool ended = false;    // +0xf4: the demo read to its end
	float fov = 90;        // degrees (demo.renderfieldofview)
	float renderWidth = 400, renderHeight = 300;
	DemoCamera camera;
	// The demo's clock as the last frame left it: the tick and the
	// fraction of the next (the demo's +0xf8, the camera's blend).
	uint32_t tick = 0;
	float fraction = 0;
	// demo.cameramode playback / record (--demo-camera /
	// --demo-record-camera): the file played, or the one recorded.
	bool playingCamera = false;
	DemoCameraTrack cameraTrack;
	DemoCameraTrack::Pose cameraPose;
	bool hasCameraPose = false;
	bool recordingCamera = false;
	std::string cameraRecordPath;
	DemoCameraRecorder cameraRecorder;
	bool cameraSaved = false;
	// The view last drawn (radians, yaw 0 along +z, pitch up positive).
	bool hasView = false;
	float viewPosition[3] = {0, 0, 0};
	float viewYaw = 0, viewPitch = 0;

	void setView(const float position[3], float yaw, float pitch)
	{
		std::copy(position, position + 3, viewPosition);
		viewYaw = yaw;
		viewPitch = pitch;
		hasView = true;
	}
	// 0x005f9630 without a local player: the demo camera's own state, its
	// mode (+0x10), followed player (+8), targetAligned (+0x88), angles
	// and distances, attached (+0xbc; the rewrite has no free look on a
	// followed player), and the view's rotation and position.
	net::CameraKeyframe cameraFrame() const
	{
		net::CameraKeyframe f;
		f.mode = camera.mode == DemoCamera::Mode::Follow ? 1u : 0u;
		f.followed = camera.followed < 0 ? 0xffffffffu : static_cast<uint32_t>(camera.followed);
		f.followFlag = camera.targetAligned ? 1 : 0;
		const float follow[5] = {camera.yaw, camera.pitch, camera.distance, camera.height, camera.side};
		std::copy(follow, follow + 5, f.follow);
		float rows[9];
		demo_camera::rowsFromYawPitch(viewYaw, viewPitch, rows);
		demo_camera::quaternionFromRows(rows, f.rotation);
		std::copy(viewPosition, viewPosition + 3, f.position);
		return f;
	}
	// The recorded camera file, written once (the original streams it and
	// closes it with the demo, 0x00516700).
	bool saveCamera()
	{
		if (!recordingCamera || cameraSaved || cameraRecordPath.empty()) {
			return false;
		}
		cameraSaved = true;
		return net::writeCameraFile(cameraRecordPath, cameraRecorder.frames());
	}
	~DemoPlayback() { saveCamera(); }

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
	// 0x005178d0 -> 0x005177b0: a new camera, the camera file from its
	// start (or recorded anew).
	void restart()
	{
		camera = {};
		ended = false;
		cameraTrack.restart();
		hasCameraPose = false;
		saveCamera(); // the file so far, closed (0x00516700) ...
		cameraRecorder.restart(); // ... and opened anew
		cameraSaved = false;
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
