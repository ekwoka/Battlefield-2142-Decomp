#include "Ik.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::anim {

namespace {

// The solver works in doubles, as 0x008c2160 does.
struct V {
	double x = 0, y = 0, z = 0;
	V operator+(const V& o) const { return {x + o.x, y + o.y, z + o.z}; }
	V operator-(const V& o) const { return {x - o.x, y - o.y, z - o.z}; }
	V operator*(double s) const { return {x * s, y * s, z * s}; }
};
double dot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V cross(const V& a, const V& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(const V& v) { return std::sqrt(dot(v, v)); }
V unit(const V& v)
{
	// 0x004481a0: a vector too short to normalise becomes zero.
	const double l2 = dot(v, v);
	return l2 < 1.1920929e-7 ? V {} : v * (1.0 / std::sqrt(l2));
}
double angleBetween(const V& a, const V& b)
{
	const double la = length(a), lb = length(b);
	if (la == 0 || lb == 0) {
		return 0;
	}
	return std::acos(std::clamp(dot(a, b) / (la * lb), -1.0, 1.0));
}
bool parallel(const V& a, const V& b)
{
	// 0x008c5b20 (tolerance 1e-10): zero vectors count as parallel.
	const V c = cross(a, b);
	return dot(c, c) <= 1e-10 * std::max(1e-30, dot(a, a) * dot(b, b)) || dot(a, a) == 0 || dot(b, b) == 0;
}

// Quaternions (x, y, z, w) in the row-vector sense: rotate(v) by q.
struct Q {
	double x = 0, y = 0, z = 0, w = 1;
};
Q axisAngle(const V& axis, double angle)
{
	const V a = unit(axis);
	const double s = std::sin(angle * 0.5);
	return {a.x * s, a.y * s, a.z * s, std::cos(angle * 0.5)};
}
V rotate(const Q& q, const V& v)
{
	const V u {q.x, q.y, q.z};
	const V t = cross(u, v) * 2.0;
	return v + t * q.w + cross(u, t);
}
// a then b.
Q then(const Q& a, const Q& b)
{
	return {b.w * a.x + b.x * a.w + b.y * a.z - b.z * a.y, b.w * a.y - b.x * a.z + b.y * a.w + b.z * a.x,
		b.w * a.z + b.x * a.y - b.y * a.x + b.z * a.w, b.w * a.w - b.x * a.x - b.y * a.y - b.z * a.z};
}
// The turn taking a onto b (0x008c57c0).
Q fromTo(const V& a, const V& b)
{
	const V axis = cross(a, b);
	if (dot(axis, axis) < 1e-20) {
		if (dot(a, b) >= 0) {
			return {};
		}
		// Opposite: half a turn about any axis square to a.
		V other = std::abs(a.x) < 0.9 ? V {1, 0, 0} : V {0, 1, 0};
		return axisAngle(cross(a, other), 3.14159265358979323846);
	}
	return axisAngle(axis, angleBetween(a, b));
}

V position(const Matrix& m) { return {m[12], m[13], m[14]}; }
void setPosition(Matrix& m, const V& p)
{
	m[12] = static_cast<float>(p.x);
	m[13] = static_cast<float>(p.y);
	m[14] = static_cast<float>(p.z);
}
// The rotation rows turned by q (R x Q, 0x006ad420).
void turn(Matrix& m, const Q& q)
{
	for (int r = 0; r < 3; ++r) {
		const V row {m[static_cast<size_t>(r * 4)], m[static_cast<size_t>(r * 4 + 1)], m[static_cast<size_t>(r * 4 + 2)]};
		const V t = rotate(q, row);
		m[static_cast<size_t>(r * 4)] = static_cast<float>(t.x);
		m[static_cast<size_t>(r * 4 + 1)] = static_cast<float>(t.y);
		m[static_cast<size_t>(r * 4 + 2)] = static_cast<float>(t.z);
	}
}
// A point through a matrix (0x004cb7d0).
V transformPoint(const Matrix& m, const V& p)
{
	return {p.x * m[0] + p.y * m[4] + p.z * m[8] + m[12], p.x * m[1] + p.y * m[5] + p.z * m[9] + m[13],
		p.x * m[2] + p.y * m[6] + p.z * m[10] + m[14]};
}

Matrix localOf(const LocalPose& pose, size_t i) { return matrixFromRotationTranslation(pose.rotations[i], pose.positions[i]); }

Matrix worldOf(const Skeleton& skeleton, const LocalPose& pose, const std::vector<Matrix>& world, size_t i)
{
	const Matrix local = localOf(pose, i);
	const int parent = skeleton.nodes[i].parent;
	return parent >= 0 && static_cast<size_t>(parent) < i ? multiply(local, world[static_cast<size_t>(parent)]) : local;
}

// Maya's solveIK (0x008c2160) with twist 0: the start joint's turn and the
// middle's.
void solve(const V& pole, const V& start, const V& mid, const V& effector, const V& handle, Q& qStart, Q& qMid)
{
	V v1 = mid - start, v2 = effector - mid;
	const V vH = handle - start;
	V vE = effector - start;
	const V vO = dot(vE, vE) > 0 ? v1 - vE * (dot(v1, vE) / dot(vE, vE)) : v1;
	const double l1 = length(v1), l2 = length(v2);
	double cosine = l1 > 0 && l2 > 0 ? (dot(vH, vH) - l1 * l1 - l2 * l2) / (2 * l1 * l2) : 1;
	cosine = std::clamp(cosine, -1.0, 1.0);
	const Q eQ = axisAngle(cross(v1, v2), std::acos(cosine) - angleBetween(v1, v2));
	v2 = rotate(eQ, v2);
	vE = v1 + v2;
	const Q qEH = fromTo(vE, vH);
	v1 = rotate(qEH, v1);
	if (parallel(v1, vH)) {
		v1 = rotate(qEH, vO);
	}
	Q qNP;
	const double h2 = dot(vH, vH);
	if (!parallel(pole, vH) && h2 != 0) {
		const V vN = v1 - vH * (dot(v1, vH) / h2);
		const V vP = pole - vH * (dot(pole, vH) / h2);
		const double ln = length(vN), lp = length(vP);
		if (ln > 0 && lp > 0 && std::abs(dot(vN, vP) / (ln * lp) + 1) < 1e-5) {
			qNP = axisAngle(vH, 3.14159265358979323846);
		} else {
			qNP = fromTo(vN, vP);
		}
	}
	qStart = then(qEH, qNP);
	qMid = eQ;
}

} // namespace

void solveArm(const Skeleton& skeleton, const LocalPose& pose, std::vector<Matrix>& world, const IkTarget& target)
{
	const int b = target.bone, end = target.end;
	if (b < 3 || end < b || static_cast<size_t>(end) >= skeleton.nodes.size()) {
		return;
	}
	const size_t B = static_cast<size_t>(b);
	// The effector: the wrist as the pose has it (B, and B + 1 when the
	// end is past B).
	world[B] = worldOf(skeleton, pose, world, B);
	size_t effector = B;
	if (end != b) {
		effector = B + 1;
		world[effector] = worldOf(skeleton, pose, world, effector);
	}
	const V e = position(world[effector]);
	const V s = position(world[B - 3]), m = position(world[B - 2]);
	const double d = length(position(world[B - 1]) - m);
	const V t = position(target.target);
	const V handle = t + unit(s - t) * d;
	const V eff = e + unit(s - e) * d;
	Q qStart, qMid;
	solve({target.pole[0], target.pole[1], target.pole[2]}, s, m, eff, handle, qStart, qMid);
	// Half the middle turn at the elbow, all of it at the lower arm
	// (0x008c26f0 halves it: the double 0.5 at 0x00964c78).
	V axis {qMid.x, qMid.y, qMid.z};
	const double sinHalf = length(axis);
	const double angle = 2 * std::atan2(sinHalf, qMid.w);
	const Q half = sinHalf > 0 ? axisAngle(axis, angle * 0.5) : Q {};
	turn(world[B - 3], qStart);
	turn(world[B - 2], then(half, qStart));
	setPosition(world[B - 2], transformPoint(world[B - 3], {pose.positions[B - 2][0], pose.positions[B - 2][1], pose.positions[B - 2][2]}));
	turn(world[B - 1], then(then(half, half), qStart));
	setPosition(world[B - 1], transformPoint(world[B - 2], {pose.positions[B - 1][0], pose.positions[B - 1][1], pose.positions[B - 1][2]}));
	if (end != b) {
		world[B] = worldOf(skeleton, pose, world, B);
	}
	world[static_cast<size_t>(end)] = target.target;
}

std::vector<Matrix> worldMatricesWithIk(const Skeleton& skeleton, const LocalPose& pose, const std::vector<IkTarget>& targets)
{
	std::vector<Matrix> world(skeleton.nodes.size());
	for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
		const IkTarget* here = nullptr;
		for (const IkTarget& t : targets) {
			if (t.bone == static_cast<int>(i)) {
				here = &t;
			}
		}
		if (here && here->end >= here->bone && static_cast<size_t>(here->end) < skeleton.nodes.size() && i >= 3) {
			solveArm(skeleton, pose, world, *here);
			i = static_cast<size_t>(here->end);
			continue;
		}
		world[i] = worldOf(skeleton, pose, world, i);
	}
	return world;
}

IkArms ikArms(const Skeleton& skeleton)
{
	IkArms arms;
	// The left arm's solve is at joint19, its wrist (left_wrist1) the end;
	// the right's both at right_ullna. 0x005efc00 passes them as numbers
	// (0x12/0x13, 0x22/0x22): the 3P skeleton's.
	const int joint19 = skeleton.find("joint19"), wrist = skeleton.find("left_wrist1"), ulna = skeleton.find("right_ullna");
	const auto chain = [&skeleton](int bone, const char* shoulder) {
		return bone >= 3 && skeleton.find(shoulder) == bone - 3;
	};
	if (joint19 >= 0 && wrist == joint19 + 1 && chain(joint19, "left_shoulder")) {
		arms.leftBone = joint19;
		arms.leftEnd = wrist;
	}
	if (ulna >= 0 && chain(ulna, "right_shoulder")) {
		arms.rightBone = arms.rightEnd = ulna;
	}
	return arms;
}

} // namespace dice::hfe::anim
