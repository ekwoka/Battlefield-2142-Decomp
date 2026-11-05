#include "AI/Formation.h"

#include <algorithm>
#include <cmath>

namespace dice::hfe::ai {

void layOutFormation(Formation& f, const std::function<float(int member)>& radius)
{
	// 0x100f58c0: the shape's step.
	switch (f.shape) {
	case 0: f.side = 1.0f; f.up = 0; f.ahead = 0; break;
	case 1: f.side = 0.8f; f.up = 0; f.ahead = -0.6f; break;
	case 2: f.side = 0.6f; f.up = 0; f.ahead = -0.8f; break;
	case 3: f.side = 0.6f; f.up = 0; f.ahead = 0.8f; break;
	default: return;
	}
	// 0x100f53a0 / 0x100f54c0 / 0x100f55f0: the same walk; only the
	// ahead component differs (0 / the step's / the step's, negated on the
	// right).
	int next = 0;
	bool right = true;
	float lateral = 0, ahead = 0;
	for (size_t i = 0; i < f.slots.size(); ++i) {
		FormationSlot& slot = f.slots[i];
		if (i != 0) {
			lateral = f.side;
			slot.parent = next;
			if (right) {
				ahead = f.shape == 0 ? 0 : (f.shape == 1 ? f.ahead : -f.ahead);
				if (i > 2) {
					++next;
				}
			} else {
				lateral = -lateral;
				ahead = f.shape == 0 ? 0 : f.ahead;
				++next;
			}
			right = !right;
		}
		slot.offset = {lateral, 0, ahead};
		if (slot.used) {
			const FormationSlot& parent = f.slots[static_cast<size_t>(slot.parent)];
			const float step = radius(parent.member) * 3.0f + radius(slot.member) * 3.0f;
			slot.offset = {step * slot.offset.x + parent.offset.x, step * slot.offset.y + parent.offset.y,
				step * slot.offset.z + parent.offset.z};
		}
	}
}

Vec3 turnOffset(const Formation& f, const Vec3& offset)
{
	float x = f.dirX, z = f.dirZ;
	const float length = std::sqrt(x * x + z * z); // 0x100643c0
	if (length > 0) {
		x /= length;
		z /= length;
	}
	float angle = std::acos(std::clamp(z, -1.0f, 1.0f)); // 0x10026930, _CIacos
	if (x > 0) {
		angle = -angle;
	}
	const float s = std::sin(angle);
	return {z * offset.x - offset.z * s, offset.y, offset.x * s + z * offset.z};
}

void dropCrumb(Formation& f)
{
	const auto length2 = [](const Vec3& v) { return v.x * v.x + v.y * v.y + v.z * v.z; };
	if (length2(f.centre) <= 0.001f) {
		return;
	}
	if (length2(f.lastCrumb) <= 0.001f) {
		f.lastCrumb = f.centre;
		return;
	}
	if (!f.slots[0].used || length2(f.centre - f.lastCrumb) <= 36.0f) {
		return;
	}
	for (size_t i = f.slots.size() - 1; i > 0; --i) {
		f.crumbs[i] = f.crumbs[i - 1];
	}
	f.crumbs[0] = {true, f.lastCrumb};
	f.lastCrumb = f.centre;
}

void markMoved(Formation& f, bool force, double now)
{
	if (!f.slots[0].used) {
		return;
	}
	f.moved = false;
	const Vec3 d = f.movedFrom - f.centre;
	if (force || (d.x * d.x + d.y * d.y + d.z * d.z > 4.0f && f.movedUntil < now)) {
		f.moved = true;
		f.movedUntil = now + 0.2;
		f.movedFrom = f.centre;
	}
}

bool hasSlot(const Formation& f, int member)
{
	return slotOf(f, member) >= 0;
}

int slotOf(const Formation& f, int member)
{
	for (size_t i = 0; i < f.slots.size(); ++i) {
		if (f.slots[i].member == member) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

void takeSlot(Formation& f, int member)
{
	for (FormationSlot& slot : f.slots) {
		if (!slot.used) {
			slot.used = true;
			slot.member = member;
			slot.crumb = -1;
			slot.lagging = false;
			slot.lagUntil = 0;
			break;
		}
	}
}

void clearSlot(FormationSlot& slot)
{
	slot.used = false;
	slot.member = -1;
	slot.crumb = -1;
	slot.lagging = false;
	slot.lagUntil = 0;
}

} // namespace dice::hfe::ai
