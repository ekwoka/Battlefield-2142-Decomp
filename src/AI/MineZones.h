// The mine manager's zones on the navigation mesh (AIDLL.dll's
// IAIMineManager, vtable 0x101145f0; BF2142.exe registers every mine of
// both teams through the AI event adaptor 0x0089f8d0 / 0x0089fdd0 ->
// slot 0x14 0x100496f0 and takes it off through slot 0x10 0x1004b880).
//
// A mine set off by soldiers (triggerType not MTYVehicle) makes a zone of
// type 0 on the infantry mesh: a 7.5 m cone, 22.5 degrees either side of
// its facing (0x10048160); one set off by vehicles (the EMP and motion
// mines) one of type 1: a 9 m circle on the vehicle mesh and a second on
// the infantry mesh. The zone is built once the mine rests (slot 3,
// 0x1004b640, every 0.5 s; IAIObjectEnvironment slot 0x70) by flooding
// the mesh from the polygon under it (0x1004ae90): the circle (0x100b94d0)
// and the triangle (0x100b9200) floods below.
//
// The marks (0x10048230, per mesh and polygon two counters: A for a
// zone's own mesh, B for a circle's second one): the first mark of a
// polygon makes its record with the counter at 1 and leaves its cost
// class; only a later mark finding the record with that counter at 0 sets
// the polygon's class to 7 (the pathfinder's slot 0x5c 0x1003f600 ->
// 0x100b8030; class 7 costs x1000, CostProfile). So in the shipped game
// only infantry polygons under both a cone and a circle turn 7 (the
// record's insert test reads as inverted). Taking a zone off (0x1004b6d0
// -> 0x100485d0 / 0x10048650) counts down and drops the record at 0 / 0;
// the class is never put back (the original byte isn't kept).
#pragma once

#include "AI/NavMesh.h"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace dice::hfe::ai {

// 0x100b94d0: the start polygon, then over the neighbours (each once)
// those with a vertex within `radius` of the centre (3-D) or an edge
// passing within it (the foot of the perpendicular inside the edge); a
// polygon left out isn't gone through.
std::vector<uint32_t> floodCircle(const NavMesh& mesh, uint32_t start, const Vec3& centre, float radius);
// 0x100b9200: on x/z, the start polygon, then the neighbours with a vertex
// inside the triangle or an edge crossing one of its edges (both
// parameters in [0, 1)); a polygon with a vertex more than 2 m above or
// below p0 is left out.
std::vector<uint32_t> floodTriangle(const NavMesh& mesh, uint32_t start, const Vec3& p0, const Vec3& p1, const Vec3& p2);

class MineMarks {
public:
	// A mark on `mesh` (0 infantry, 1 vehicles), counter A or B.
	void mark(NavMesh& navMesh, int mesh, uint32_t polygon, bool counterB);
	void unmark(int mesh, uint32_t polygon, bool counterB);
	bool marked(int mesh, uint32_t polygon) const { return m_records.count({mesh, polygon}) != 0; }

private:
	std::map<std::pair<int, uint32_t>, std::pair<int, int>> m_records; // +0x24: (A, B)
};

} // namespace dice::hfe::ai
