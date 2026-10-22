#include "RagDoll.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace dice::hfe::anim {

namespace {

Vec operator+(const Vec& a, const Vec& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec operator-(const Vec& a, const Vec& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec operator*(const Vec& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec& operator+=(Vec& a, const Vec& b)
{
	a = a + b;
	return a;
}
Vec& operator-=(Vec& a, const Vec& b)
{
	a = a - b;
	return a;
}
float dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float length(const Vec& a) { return std::sqrt(dot(a, a)); }
// 0x005a2b60
Vec cross(const Vec& a, const Vec& b)
{
	return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
// 0x004481a0: unchanged when (nearly) zero.
Vec normalized(const Vec& a)
{
	const float l = length(a);
	return l > 1e-12f ? a * (1 / l) : a;
}
Vec translation(const Matrix& m) { return {m[12], m[13], m[14]}; }

// 0x006bd360: the closest points of two segments, as fractions along them.
float segmentDistance(const Vec& p1, const Vec& p2, const Vec& q1, const Vec& q2, float& s, float& t)
{
	const Vec d1 = p2 - p1, d2 = q2 - q1, r = p1 - q1;
	const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
	if (a <= 1e-12f && e <= 1e-12f) {
		s = t = 0;
	} else if (a <= 1e-12f) {
		s = 0;
		t = std::clamp(f / e, 0.0f, 1.0f);
	} else {
		const float c = dot(d1, r);
		if (e <= 1e-12f) {
			t = 0;
			s = std::clamp(-c / a, 0.0f, 1.0f);
		} else {
			const float b = dot(d1, d2), denom = a * e - b * b;
			s = denom > 1e-12f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0;
			t = (b * s + f) / e;
			if (t < 0) {
				t = 0;
				s = std::clamp(-c / a, 0.0f, 1.0f);
			} else if (t > 1) {
				t = 1;
				s = std::clamp((b - c) / a, 0.0f, 1.0f);
			}
		}
	}
	return length((p1 + d1 * s) - (q1 + d2 * t));
}

// Pulls (or pushes) two particles to `rest` apart, by their masses
// (0x008a8c40's step; the angle constraints reuse it).
void relax(Vec& a, float inverseA, Vec& b, float inverseB, float rest)
{
	const Vec diff = b - a;
	const float d = length(diff);
	if (d > 0.0001f) {
		const float k = (d - rest) / ((inverseB + inverseA) * d);
		a += diff * (k * inverseA);
		b -= diff * (k * inverseB);
	}
}

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

} // namespace

RagDollTemplate::RagDollTemplate()
{
	std::fill(std::begin(m_particleOf), std::end(m_particleOf), -1);
}

void RagDollTemplate::setSkeleton(const Skeleton* skeleton)
{
	m_skeleton = skeleton;
	m_reference.clear();
	if (skeleton) {
		for (const Matrix& m : worldMatrices(*skeleton, setupPose(*skeleton))) {
			m_reference.push_back(translation(m));
		}
	}
}

float RagDollTemplate::referenceDistance(int a, int b) const
{
	if (a < 0 || b < 0 || static_cast<size_t>(a) >= m_reference.size() || static_cast<size_t>(b) >= m_reference.size()) {
		return 0;
	}
	return length(m_reference[static_cast<size_t>(a)] - m_reference[static_cast<size_t>(b)]);
}

int RagDollTemplate::particleOf(int bone) const
{
	return bone >= 0 && bone < 128 ? m_particleOf[bone] : -1;
}

bool RagDollTemplate::locked(int bone) const
{
	return bone >= 0 && static_cast<size_t>(bone) < m_locked.size() && m_locked[static_cast<size_t>(bone)];
}

// Knees get ragdoll_kneeBoneSize, hips and shoulders ragdoll_bodyBoneSize,
// the rest ragdoll_boneSize (read when the particle is added).
void RagDollTemplate::addParticle(int bone, float mass)
{
	if (bone < 0 || bone >= 128) {
		return;
	}
	float radius = settings.boneSize;
	if (bone == 2 || bone == 7) {
		radius = settings.kneeBoneSize;
	} else if (bone == 6 || bone == 31 || bone == 1 || bone == 15) {
		radius = settings.bodyBoneSize;
	}
	m_particleOf[bone] = static_cast<int>(m_particles.size());
	m_particles.push_back({bone, mass > 0 ? mass : 1, radius});
}

// A pair already constrained keeps its place and takes the new length.
void RagDollTemplate::addConstraint(int boneA, int boneB)
{
	const int a = particleOf(boneA), b = particleOf(boneB);
	if (a < 0 || b < 0) {
		return;
	}
	const float rest = referenceDistance(boneA, boneB);
	for (Distance& existing : m_constraints) {
		if (existing.a == a && existing.b == b) {
			existing.length = rest;
			return;
		}
	}
	m_constraints.push_back({a, b, rest});
}

void RagDollTemplate::addDistanceLessThanConstraint(int boneA, int boneB, float distance)
{
	const int a = particleOf(boneA), b = particleOf(boneB);
	if (a >= 0 && b >= 0) {
		m_lessThan.push_back({a, b, distance});
	}
}

// The angle at b becomes a distance a-c by the law of cosines; a negative
// angle makes it a maximum. The same triple again replaces the limit (so
// ragDollConstraints.inc's "1 2 4 110" then "1 2 4 -150" leaves -150).
void RagDollTemplate::addAngularConstraint(int boneA, int boneB, int boneC, float degrees)
{
	const int a = particleOf(boneA), b = particleOf(boneB), c = particleOf(boneC);
	if (a < 0 || b < 0 || c < 0) {
		return;
	}
	const float ab = referenceDistance(boneA, boneB), bc = referenceDistance(boneB, boneC);
	const float cosine = std::cos(std::fabs(degrees) * 0.017453292f);
	float limit = std::sqrt(std::max(0.0f, ab * ab + bc * bc - 2 * bc * ab * cosine));
	if (degrees < 0) {
		limit = -limit;
	}
	for (Angular& existing : m_angular) {
		if (existing.a == a && existing.b == b && existing.c == c) {
			existing.limit = limit;
			return;
		}
	}
	m_angular.push_back({a, b, c, ab, bc, limit});
}

void RagDollTemplate::addDihedralAngleConstraint(int boneA, int boneB, int boneC, int boneD, float degrees)
{
	const int a = particleOf(boneA), b = particleOf(boneB), c = particleOf(boneC), d = particleOf(boneD);
	if (a < 0 || b < 0 || c < 0 || d < 0) {
		return;
	}
	const float ab = referenceDistance(boneA, boneB), bc = referenceDistance(boneB, boneC),
				cd = referenceDistance(boneC, boneD);
	const float cosine = std::cos(degrees * 0.017453292f);
	const float limit = std::sqrt(std::max(0.0f, ab * ab + cd * cd - 2 * cd * ab * cosine));
	for (Dihedral& existing : m_dihedral) {
		if (existing.a == a && existing.b == b && existing.c == c && existing.d == d) {
			existing.limit = limit;
			return;
		}
	}
	m_dihedral.push_back({a, b, c, d, ab, bc, cd, limit});
}

void RagDollTemplate::removeConstraint(int boneA, int boneB)
{
	const int a = particleOf(boneA), b = particleOf(boneB);
	m_constraints.erase(std::remove_if(m_constraints.begin(), m_constraints.end(),
							[&](const Distance& d) { return d.a == a && d.b == b; }),
		m_constraints.end());
}

void RagDollTemplate::addCapsuleCollision(int boneA, int boneB)
{
	const int a = particleOf(boneA), b = particleOf(boneB);
	if (a >= 0 && b >= 0) {
		m_capsules.push_back({a, b});
	}
}

void RagDollTemplate::addJointConstraint(int boneA, int boneB)
{
	const int a = particleOf(boneA), b = particleOf(boneB);
	if (a >= 0 && b >= 0) {
		m_joints.push_back({a, b});
	}
}

void RagDollTemplate::lockBone(int from, int to)
{
	for (int bone = std::max(from, 0); bone <= to; ++bone) {
		if (static_cast<size_t>(bone) >= m_locked.size()) {
			m_locked.resize(static_cast<size_t>(bone) + 1, false);
		}
		m_locked[static_cast<size_t>(bone)] = true;
	}
}

void RagDollTemplate::toSkeleton(int bone, int a, int b) { m_links.push_back({bone, a, b}); }

void RagDollTemplate::setParticleCollisionCriteria(float minSpeed, float minDelay)
{
	m_collisionMinSpeed = minSpeed;
	m_collisionMinDelay = minDelay;
}

bool RagDollTemplate::execute(const std::string& line, std::string* include, std::string* skeletonPath)
{
	std::istringstream in(line);
	std::string command;
	if (!(in >> command)) {
		return true;
	}
	const std::string name = lower(command);
	if (name == "rem" || name.compare(0, 2, "//") == 0) {
		return true;
	}
	std::vector<std::string> args;
	for (std::string arg; in >> arg;) {
		args.push_back(arg);
	}
	const auto i = [&](size_t n) { return n < args.size() ? std::atoi(args[n].c_str()) : 0; };
	const auto f = [&](size_t n) { return n < args.size() ? static_cast<float>(std::atof(args[n].c_str())) : 0.0f; };
	if (name == "include" || name == "run") {
		if (include && !args.empty()) {
			*include = args[0];
		}
		return true;
	}
	if (name == "vars.set") {
		if (args.size() < 2) {
			return false;
		}
		const std::string var = lower(args[0]);
		const float value = f(1);
		struct Var {
			const char* name;
			float* number;
		};
		const Var floats[] = {{"ragdoll_dt", &settings.dT}, {"ragdoll_slowmotion", &settings.slowMotion},
			{"ragdoll_gravityovertime", &settings.gravityOverTime}, {"ragdoll_sleep", &settings.sleep},
			{"ragdoll_sleepdelta", &settings.sleepDelta}, {"ragdoll-max-inherit-speed", &settings.maxInheritSpeed},
			{"ragdoll_exprotateforce", &settings.expRotateForce}, {"ragdoll_bonesize", &settings.boneSize},
			{"ragdoll_kneebonesize", &settings.kneeBoneSize}, {"ragdoll_bodybonesize", &settings.bodyBoneSize},
			{"ragdoll_completecollradius", &settings.completeCollRadius},
			{"ragdoll_legbackangle", &settings.legBackAngle}, {"ragdoll_legforwardangle", &settings.legForwardAngle},
			{"ragdoll_legforwardfactor", &settings.legForwardFactor},
			{"ragdoll_legpointforwardangle", &settings.legPointForwardAngle},
			{"ragdoll_legpointforwardfactor", &settings.legPointForwardFactor},
			{"rd_expforcemod", &settings.expForceMod}, {"rd_dieforce", &settings.dieForce}};
		for (const Var& v : floats) {
			if (var == v.name) {
				*v.number = value;
				return true;
			}
		}
		struct Flag {
			const char* name;
			bool* flag;
		};
		const Flag flags[] = {{"ragdoll_enableforce", &settings.enableForce},
			{"ragdoll_useconstraints", &settings.useConstraints},
			{"ragdoll_useangularconstraints", &settings.useAngularConstraints},
			{"ragdoll_uselandcollision", &settings.useLandCollision},
			{"ragdoll_useobjectcollision", &settings.useObjectCollision},
			{"ragdoll_usecapsulecollision", &settings.useCapsuleCollision}};
		for (const Flag& v : flags) {
			if (var == v.name) {
				*v.flag = value != 0;
				return true;
			}
		}
		if (var == "ragdoll_iterations") {
			settings.iterations = i(1);
			return true;
		}
		return false;
	}
	if (name.compare(0, 8, "ragdoll.") != 0) {
		return false;
	}
	const std::string method = name.substr(8);
	if (method == "skeleton") {
		if (skeletonPath && !args.empty()) {
			*skeletonPath = args[0];
		}
	} else if (method == "gravity") {
		gravity = f(0);
	} else if (method == "addparticle") {
		addParticle(i(0), f(1));
	} else if (method == "addconstraint") {
		addConstraint(i(0), i(1));
	} else if (method == "adddistancelessthanconstraint") {
		addDistanceLessThanConstraint(i(0), i(1), f(2));
	} else if (method == "addangularconstraint") {
		addAngularConstraint(i(0), i(1), i(2), f(3));
	} else if (method == "adddihedralangleconstraint") {
		addDihedralAngleConstraint(i(0), i(1), i(2), i(3), f(4));
	} else if (method == "removeconstraint") {
		removeConstraint(i(0), i(1));
	} else if (method == "addcapsulecollision") {
		addCapsuleCollision(i(0), i(1));
	} else if (method == "addjointconstraint") {
		addJointConstraint(i(0), i(1));
	} else if (method == "lockbone") {
		lockBone(i(0), i(1));
	} else if (method == "toskeleton") {
		toSkeleton(i(0), i(1), i(2));
	} else if (method == "setparticlecollisioncriteria") {
		setParticleCollisionCriteria(f(0), f(1));
	} else if (method == "addbone" || method == "linkmode" || method == "active" || method == "drawconstraints"
		|| method == "reset" || method == "save" || method == "addforce") {
		// Bone collision ("not used right now"), debugging and editing.
	} else {
		return false;
	}
	return true;
}

bool loadRagDoll(RagDollTemplate& ragDoll, const std::string& path,
	const std::function<bool(const std::string& path, std::vector<uint8_t>& data)>& read,
	const std::function<const Skeleton*(const std::string& path)>& skeleton)
{
	const auto slashes = [](std::string p) {
		std::replace(p.begin(), p.end(), '\\', '/');
		return p;
	};
	const std::function<bool(const std::string&, int)> run = [&](const std::string& file, int depth) {
		std::vector<uint8_t> data;
		if (depth > 8 || !read(file, data)) {
			return false;
		}
		const std::string directory = file.substr(0, file.find_last_of('/') + 1);
		std::istringstream lines(std::string(data.begin(), data.end()));
		for (std::string line; std::getline(lines, line);) {
			std::string include, skeletonPath;
			ragDoll.execute(line, &include, &skeletonPath);
			if (!skeletonPath.empty()) {
				ragDoll.setSkeleton(skeleton(slashes(skeletonPath)));
			}
			if (!include.empty()) {
				run(directory + slashes(include), depth + 1);
			}
		}
		return true;
	};
	return run(slashes(path), 0) && ragDoll.skeleton() && ragDoll.midParticle() >= 0;
}

RagDoll::Particle* RagDoll::particleOfBone(int bone)
{
	const int index = m_template ? m_template->particleOf(bone) : -1;
	return index >= 0 ? &m_particles[static_cast<size_t>(index)] : nullptr;
}

const RagDoll::Particle* RagDoll::particleOfBone(int bone) const
{
	const int index = m_template ? m_template->particleOf(bone) : -1;
	return index >= 0 ? &m_particles[static_cast<size_t>(index)] : nullptr;
}

// The instance starts awake with a full sleep countdown; the particles'
// previous positions are 0.015 s of the velocity back (0x008aca60).
void RagDoll::start(const RagDollTemplate& ragDoll, const std::vector<Matrix>& pose, Vec velocity)
{
	m_template = &ragDoll;
	const RagDollSettings& s = ragDoll.settings;
	const float speed = length(velocity);
	if (speed > s.maxInheritSpeed) {
		velocity = velocity * (s.maxInheritSpeed / speed);
	}
	m_particles.clear();
	for (const RagDollTemplate::Particle& p : ragDoll.particles()) {
		Particle particle {};
		particle.position = p.bone >= 0 && static_cast<size_t>(p.bone) < pose.size()
			? translation(pose[static_cast<size_t>(p.bone)])
			: Vec {};
		particle.previous = particle.position - velocity * 0.015f;
		particle.bone = p.bone;
		particle.mass = p.mass;
		particle.radius = p.radius;
		m_particles.push_back(particle);
	}
	m_forced = false;
	m_awake = true;
	m_sleepLeft = s.sleep;
	m_accumulated = 0;
	m_gravityRamp = s.gravityOverTime;
	m_gravityTime = 0;
	m_last = middle();
}

const Vec& RagDoll::middle() const
{
	static const Vec none {};
	const Particle* mid = particleOfBone(6);
	return mid ? mid->position : none;
}

Vec RagDoll::bodyNormal() const
{
	const Particle *hipL = particleOfBone(1), *hipR = particleOfBone(6), *shoulder = particleOfBone(31);
	if (!hipL || !hipR || !shoulder) {
		return {0, 1, 0};
	}
	const Vec a = shoulder->position - hipL->position;
	const Vec b = hipR->position - shoulder->position;
	return normalized(cross(b, a));
}

Vec RagDoll::hipAxis() const
{
	const Particle *hipL = particleOfBone(1), *hipR = particleOfBone(6);
	return hipL && hipR ? normalized(hipL->position - hipR->position) : Vec {1, 0, 0};
}

void RagDoll::addForce(int bone, const Vec& force, bool again)
{
	if (!m_template || !m_template->settings.enableForce || (m_forced && !again)) {
		return;
	}
	m_forced = true;
	m_sleepLeft = m_template->settings.sleep;
	m_awake = true;
	const float scale = m_template->settings.expRotateForce;
	if (bone < 0) {
		const Vec turn = bodyNormal() * scale;
		for (Particle& p : m_particles) {
			if (p.bone == 4 || p.bone == 9 || p.bone == 2 || p.bone == 7 || p.bone == 1 || p.bone == 6) {
				p.force -= turn;
			} else {
				p.force += force * scale;
			}
		}
	} else if (Particle* p = particleOfBone(bone)) {
		p->force += force;
	}
}

void RagDoll::accumulate()
{
	const float ramp = m_gravityRamp > 0 ? m_gravityTime / m_gravityRamp : 1;
	for (Particle& p : m_particles) {
		p.acceleration += p.force * (1 / p.mass);
		p.acceleration[1] += ramp * m_template->gravity;
	}
}

// Verlet, averaging this step's displacement with the last one's.
void RagDoll::integrate(float step)
{
	for (Particle& p : m_particles) {
		const Vec move = (p.position - p.previous) + p.acceleration * (step * step);
		p.previous = p.position;
		p.position += (move + p.lastMove) * 0.5f;
		p.lastMove = move;
		p.acceleration = {};
		p.force = {};
	}
}

void RagDoll::distanceConstraints()
{
	for (const RagDollTemplate::Distance& c : m_template->constraints()) {
		Particle& a = m_particles[static_cast<size_t>(c.a)];
		Particle& b = m_particles[static_cast<size_t>(c.b)];
		relax(a.position, 1 / a.mass, b.position, 1 / b.mass, c.length);
	}
}

// Only pushes apart.
void RagDoll::lessThanConstraints()
{
	for (const RagDollTemplate::Distance& c : m_template->lessThan()) {
		Particle& a = m_particles[static_cast<size_t>(c.a)];
		Particle& b = m_particles[static_cast<size_t>(c.b)];
		if (length(b.position - a.position) < c.length) {
			relax(a.position, 1 / a.mass, b.position, 1 / b.mass, c.length);
		}
	}
}

// Keeps a-c within the limit; a correction is followed by putting a-b and
// b-c back to length (with the inverse masses as the game mixes them).
void RagDoll::angularConstraints()
{
	if (!m_template->settings.useAngularConstraints) {
		return;
	}
	for (const RagDollTemplate::Angular& c : m_template->angular()) {
		Vec& a = m_particles[static_cast<size_t>(c.a)].position;
		Vec& b = m_particles[static_cast<size_t>(c.b)].position;
		Vec& pc = m_particles[static_cast<size_t>(c.c)].position;
		const float ia = 1 / m_particles[static_cast<size_t>(c.a)].mass;
		const float ic = 1 / m_particles[static_cast<size_t>(c.c)].mass;
		const float d = length(pc - a);
		if (d <= 0.0001f) {
			continue;
		}
		float target = c.limit, low = c.limit, high = 9999;
		if (c.limit < 0) {
			target = -c.limit;
			low = 0;
			high = target;
		}
		if (d >= low && d <= high) {
			continue;
		}
		relax(a, ia, pc, ic, target);
		// a-b: b moves by c's inverse mass.
		{
			const Vec diff = a - b;
			const float l = length(diff);
			if (l > 0.0001f) {
				const float k = (l - c.ab) / ((ic + ia) * l);
				b += diff * (k * ic);
				a -= diff * (k * ia);
			}
		}
		// b-c: both by c's.
		{
			const Vec diff = b - pc;
			const float l = length(diff);
			if (l > 0.0001f) {
				const float k = (l - c.bc) / ((ic + ic) * l);
				pc += diff * (k * ic);
				b -= diff * (k * ic);
			}
		}
	}
}

// The arms a-b and c-d may not fold together closer than the limit (their
// sum's length); then all three bones are put back to length.
void RagDoll::dihedralConstraints()
{
	if (!m_template->settings.useAngularConstraints) {
		return;
	}
	for (const RagDollTemplate::Dihedral& c : m_template->dihedral()) {
		Particle& pa = m_particles[static_cast<size_t>(c.a)];
		Particle& pb = m_particles[static_cast<size_t>(c.b)];
		Particle& pc = m_particles[static_cast<size_t>(c.c)];
		Particle& pd = m_particles[static_cast<size_t>(c.d)];
		const Vec v = (pd.position - pc.position) + pb.position - pa.position;
		const float l = length(v);
		if (l <= 0.0001f || l >= c.limit) {
			continue;
		}
		const float ia = 1 / pa.mass, ib = 1 / pb.mass, ic = 1 / pc.mass, id = 1 / pd.mass;
		const float k = (l - c.limit) / ((id + ia) * l);
		pa.position += v * (k * ia);
		pd.position -= v * (k * id);
		// relax(x, y) pulls x towards y: the game's (b, a), (d, c), (c, b).
		relax(pb.position, ib, pa.position, ia, c.ab);
		relax(pd.position, id, pc.position, ic, c.cd);
		relax(pc.position, ic, pb.position, ib, c.bc);
	}
}

// For each leg (hips 1 and 6, knees +1, feet +3): the thigh kept between
// legBackAngle and legForwardAngle of the body's normal, and the shin
// pointing forward (legPointForward*).
void RagDoll::legConstraints()
{
	const RagDollSettings& s = m_template->settings;
	const Vec normal = bodyNormal();
	const Vec axis = hipAxis();
	for (const int hipBone : {1, 6}) {
		Particle *hip = particleOfBone(hipBone), *knee = particleOfBone(hipBone + 1), *foot = particleOfBone(hipBone + 3);
		if (!hip || !knee || !foot) {
			continue;
		}
		const Vec thigh = knee->position - hip->position;
		const Vec side = normalized(cross(axis, thigh));
		const float forward = dot(normalized(thigh), normal);
		if (s.legForwardFactor != 0) {
			const float* limit = forward < s.legBackAngle ? &s.legBackAngle : s.legForwardAngle < forward ? &s.legForwardAngle : nullptr;
			if (limit) {
				const float push = (*limit - forward) * s.legForwardFactor;
				hip->position -= side * push;
				knee->position += side * push;
			}
		}
		if (s.legPointForwardFactor != 0) {
			const Vec shin = normalized(foot->position - knee->position);
			const float pointing = -dot(shin, side);
			if (pointing < s.legPointForwardAngle) {
				const float push = (s.legPointForwardAngle - pointing) * s.legPointForwardFactor;
				knee->position += side * push;
				foot->position -= side * push;
			}
		}
	}
}

// The shins (knee-foot) may not pass through each other.
void RagDoll::legCollision()
{
	Particle *kneeL = particleOfBone(2), *footL = particleOfBone(4), *kneeR = particleOfBone(7), *footR = particleOfBone(9);
	if (!kneeL || !footL || !kneeR || !footR) {
		return;
	}
	float s = 0, t = 0;
	const float d = segmentDistance(kneeL->position, footL->position, kneeR->position, footR->position, s, t);
	const float depth = d - 2 * m_template->settings.boneSize;
	if (d < 1.1920929e-07f || depth > 0) {
		return;
	}
	const Vec c1 = kneeL->position + (footL->position - kneeL->position) * s;
	const Vec c2 = kneeR->position + (footR->position - kneeR->position) * t;
	const Vec push = (c1 - c2) * (1 / d) * (-depth * 0.25f);
	kneeL->position += push;
	footL->position += push;
	kneeR->position -= push;
	footR->position -= push;
}

// Particles against objects, then the terrain: a segment from where the
// particle was (or from half its radius above it, if it hasn't moved) to a
// radius past where it is now. A particle moving into a face is put back so
// that segment's end touches the face, and stops (its previous position is
// set to the new one).
void RagDoll::collide(const RagDollWorld& world)
{
	const RagDollSettings& s = m_template->settings;
	legCollision();
	const auto segment = [](const Particle& p, Vec& from, Vec& direction, Vec& to) {
		from = p.previous;
		const Vec moved = p.position - p.previous;
		if (dot(moved, moved) <= 1.1920929e-07f) {
			from = p.position;
			from[1] += p.radius * 0.5f;
		}
		direction = normalized(p.position - from);
		to = p.position + direction * p.radius;
	};
	if (s.useObjectCollision && world.raycast) {
		for (Particle& p : m_particles) {
			p.hitObject = false;
			Vec from, direction, to, point, normal;
			segment(p, from, direction, to);
			if (world.raycast(from, to, point, normal) && -dot(normal, direction) > 0) {
				p.hitObject = true;
				p.position -= normal * dot(to - point, normal);
				p.previous = p.position;
			}
		}
	}
	if (s.useLandCollision && world.ground) {
		for (Particle& p : m_particles) {
			if (p.hitObject) {
				p.hitObject = false;
				continue;
			}
			Vec from, direction, to, normal;
			segment(p, from, direction, to);
			const float startHeight = world.ground(from[0], from[2], normal);
			const float endHeight = world.ground(to[0], to[2], normal);
			if (from[1] >= startHeight && to[1] < endHeight && -dot(normal, direction) > 0) {
				// Where the segment enters the ground (bisection).
				float lo = 0, hi = 1;
				for (int i = 0; i < 12; ++i) {
					const float mid = (lo + hi) * 0.5f;
					const Vec at = from + (to - from) * mid;
					Vec n;
					(at[1] < world.ground(at[0], at[2], n) ? hi : lo) = mid;
				}
				Vec point = from + (to - from) * hi;
				point[1] = world.ground(point[0], point[2], normal);
				p.position -= normal * dot(to - point, normal);
				p.previous = p.position;
			} else {
				// Started inside or moving out: lifted 1% of the way a step.
				const float height = world.ground(p.position[0], p.position[2], normal);
				if (height > p.position[1]) {
					p.position[1] += (height - p.position[1]) * 0.01f;
					p.previous = p.position;
				}
			}
		}
	}
	capsuleCollision(world);
}

// The body's capsules (addCapsuleCollision) against objects, while
// ragdoll_useCapsuleCollision is set (0x008abf20): for each object in the
// body's list (see GameWorld::ragDollWorld) and each capsule, the object
// sweeps particle a's sphere (a's radius) from a to b through its mesh
// (object slot 0x7c, the collision mesh's swept-sphere test 0x006e2350 at
// LOD 2); every contact deeper than 0 moves both ends out by its depth
// along its normal and stops them (previous = position). Not rewritten:
// the sweep (the collision world has no swept-sphere query yet), so this
// tests a sphere of a's radius at the capsule's middle and takes the
// deepest contact only.
void RagDoll::capsuleCollision(const RagDollWorld& world)
{
	if (!m_template->settings.useCapsuleCollision || !world.sphere) {
		return;
	}
	for (const RagDollTemplate::Pair& c : m_template->capsules()) {
		Particle& a = m_particles[static_cast<size_t>(c.a)];
		Particle& b = m_particles[static_cast<size_t>(c.b)];
		Vec normal;
		float depth = 0;
		if (world.sphere((a.position + b.position) * 0.5f, a.radius, normal, depth) && depth < 0) {
			for (Particle* p : {&a, &b}) {
				p->position += normal * -depth;
				p->previous = p->position;
			}
		}
	}
}

void RagDoll::solve(const RagDollWorld& world)
{
	const RagDollSettings& s = m_template->settings;
	for (int i = 0; i < s.iterations; ++i) {
		if (s.useConstraints) {
			angularConstraints();
			dihedralConstraints();
			legConstraints();
			lessThanConstraints();
			distanceConstraints();
		}
		collide(world);
	}
}

const Vec& RagDoll::update(float seconds, const RagDollWorld& world)
{
	if (!m_template || m_particles.empty()) {
		return middle();
	}
	const RagDollSettings& s = m_template->settings;
	const float step = s.dT;
	seconds *= s.slowMotion;
	if (seconds < 0 || !m_awake) {
		return middle();
	}
	m_accumulated += seconds;
	if (step > 0 && m_accumulated >= step) {
		const int steps = static_cast<int>(std::nearbyint(m_accumulated / step));
		m_accumulated -= static_cast<float>(steps) * step;
		for (int i = 0; i < steps && i < 64; ++i) {
			if (m_gravityRamp > 0) {
				m_gravityTime = std::min(m_gravityTime + step, m_gravityRamp);
			}
			accumulate();
			integrate(step);
			solve(world);
		}
	}
	// Asleep after ragdoll_sleep seconds of moving less than sleepDelta a frame.
	const Vec mid = middle();
	if (s.sleep > 0) {
		const Vec moved = mid - m_last;
		if (dot(moved, moved) <= s.sleepDelta * s.sleepDelta) {
			m_sleepLeft -= seconds;
			if (m_sleepLeft <= 0) {
				m_sleepLeft = 0;
				m_awake = false;
			}
		} else {
			m_sleepLeft = s.sleep;
		}
	}
	m_last = mid;
	return middle();
}

void RagDoll::pose(const Skeleton& skeleton, const LocalPose& local, std::vector<Matrix>& matrices) const
{
	if (!m_template || m_particles.empty()) {
		return;
	}
	matrices.resize(skeleton.nodes.size());
	const auto node = [&](int bone) -> Matrix& {
		static Matrix none {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
		return bone >= 0 && static_cast<size_t>(bone) < matrices.size() ? matrices[static_cast<size_t>(bone)] : none;
	};
	const auto localMatrix = [&](int bone) {
		const size_t i = static_cast<size_t>(bone);
		return i < local.rotations.size() ? matrixFromRotationTranslation(local.rotations[i], local.positions[i])
										  : matrixFromRotationTranslation(skeleton.nodes[i].rotation, skeleton.nodes[i].position);
	};
	// Particle positions relative to the middle one.
	const Vec mid = middle();
	std::vector<Vec> at;
	for (const Particle& p : m_particles) {
		at.push_back(p.position - mid);
	}
	const auto rel = [&](int bone) { return at[static_cast<size_t>(m_template->particleOf(bone))]; };
	const Vec normal = bodyNormal();
	const Vec axis = hipAxis();
	Vec legNormal {1, 0, 0}, reference {1, 0, 0};
	const auto planeNormal = [&](int a, int b, int c) {
		if (m_template->particleOf(a) < 0 || m_template->particleOf(b) < 0 || m_template->particleOf(c) < 0) {
			return Vec {1, 0, 0};
		}
		return normalized(cross(rel(b) - rel(a), rel(c) - rel(a)));
	};
	for (const RagDollTemplate::Link& link : m_template->links()) {
		const int bone = std::abs(link.bone);
		if (static_cast<size_t>(bone) >= matrices.size()) {
			continue;
		}
		const Matrix current = node(bone);
		const int self = m_template->particleOf(bone);
		const int pa = m_template->particleOf(std::abs(link.a));
		const int pb = m_template->particleOf(std::abs(link.b));
		Vec position = translation(current);
		Vec y;
		if (self < 0) {
			const Vec from = pa >= 0 ? at[static_cast<size_t>(pa)] : translation(node(std::abs(link.a)));
			const Vec to = pb >= 0 ? at[static_cast<size_t>(pb)] : translation(node(std::abs(link.b)));
			y = normalized(to - from);
			// Both given as indices (not negative): placed along a-b at its
			// setup distance from a.
			if (link.a >= 0 && link.b >= 0) {
				position = from + y * m_template->referenceDistance(std::abs(link.a), bone);
			}
		} else {
			if (link.a >= 0 && link.b >= 0) {
				position = at[static_cast<size_t>(self)];
			}
			if (pa < 0 || pb < 0) {
				continue;
			}
			y = normalized(at[static_cast<size_t>(pb)] - at[static_cast<size_t>(pa)]);
			if (bone == 1 || bone == 6) {
				const int knee = m_template->particleOf(bone + 1), foot = m_template->particleOf(bone + 3);
				if (knee >= 0 && foot >= 0) {
					legNormal = normalized(cross(at[static_cast<size_t>(knee)] - position, at[static_cast<size_t>(foot)] - position));
				}
			}
		}
		// The bone's x axis: the leg's plane, the hips' axis along the spine,
		// the body's normal for the collars, the arm's plane for the arms.
		if ((bone > 5 && bone < 10) || (bone > 0 && bone < 5)) {
			reference = legNormal;
		}
		if (bone == 0 || bone == 11 || bone == 12 || bone == 13 || bone == 45 || bone == 46 || bone == 47) {
			reference = axis * -1;
		}
		if (bone == 30) {
			reference = normal;
		}
		if (bone == 14) {
			reference = normal * -1;
		}
		if (bone >= 31 && bone <= 34) {
			reference = cross(planeNormal(31, 32, 34), y);
		}
		if (bone >= 15 && bone <= 19) {
			reference = cross(planeNormal(15, 16, 19), y);
		}
		// 0x006ac530: z = x * y, then y = z * x and x = y * z.
		Vec x = normalized(reference);
		y = normalized(y);
		const Vec z = normalized(cross(x, y));
		y = normalized(cross(z, x));
		x = normalized(cross(y, z));
		Matrix m {x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, position[0], position[1], position[2], 1};
		if (link.a == link.b) {
			// Keeps its rotation relative to its parent.
			const int parent = skeleton.nodes[static_cast<size_t>(std::abs(link.a))].parent;
			m = multiply(localMatrix(std::abs(link.a)), parent >= 0 ? node(parent) : Matrix {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
			m[12] = position[0];
			m[13] = position[1];
			m[14] = position[2];
		}
		node(bone) = m;
	}
	// Locked bones (fingers, face, the weapon's mesh nodes) follow their parents.
	for (size_t i = 0; i < skeleton.nodes.size(); ++i) {
		const int parent = skeleton.nodes[i].parent;
		if (m_template->locked(static_cast<int>(i)) && parent >= 0) {
			matrices[i] = multiply(localMatrix(static_cast<int>(i)), matrices[static_cast<size_t>(parent)]);
		}
	}
}

} // namespace dice::hfe::anim
