// Which render queue a material takes and the transparency queues' order
// (RendDX9 0x100c3880, 0x10039380; src/Render/MeshQueues.h).
#include "Render/MeshQueues.h"
#include "Geometry/GeometryTemplateManager.h"
#include "Console/Console.h"
#include "TestMain.h"

#include <string>

using namespace dice::hfe;

TEST(MeshQueues_MaterialsByAlphaModeAndFade)
{
	using render::MeshQueue;
	CHECK(render::meshQueue(false, false, 1.0f) == MeshQueue::Opaque);
	CHECK(render::meshQueue(true, false, 1.0f) == MeshQueue::Transparent);
	// hasAdditiveAlpha only changes blended materials, faded or not.
	CHECK(render::meshQueue(true, true, 1.0f) == MeshQueue::Additive);
	CHECK(render::meshQueue(true, true, 0.5f) == MeshQueue::Additive);
	CHECK(render::meshQueue(false, true, 1.0f) == MeshQueue::Opaque);
	// An object fading out sends its opaque materials to the transparency queue.
	CHECK(render::meshQueue(false, false, 0.5f) == MeshQueue::Transparent);
}

TEST(MeshQueues_BackToFrontByObjectDistance)
{
	const float camera[3] = {0, 0, 0};
	const float near[3] = {0, 0, 2}, far[3] = {0, 3, 4};
	CHECK(render::transparencyKey(near, camera) == 2.0f && render::transparencyKey(far, camera) == 5.0f);
	std::vector<std::pair<float, int>> items {{2, 1}, {5, 2}, {2, 3}, {9, 4}};
	render::sortBackToFront(items);
	// Farthest first; one object's materials (equal keys) keep their order.
	CHECK(items[0].second == 4 && items[1].second == 2 && items[2].second == 1 && items[3].second == 3);
	CHECK(render::scrollOffset(-0.2f, 1.5f) > 0.69f && render::scrollOffset(-0.2f, 1.5f) < 0.71f);
	CHECK(render::scrollOffset(0, 10) == 0);
}

TEST(MeshQueues_GeometryTemplatesAdditiveAlpha)
{
	Console console;
	geom::GeometryTemplateManager geometries;
	geom::registerGeometryConsoleMethods(console, geometries);
	std::string output;
	console.execute("GeometryTemplate.create BundledMesh eu_ar_rifle", output, ConsoleAccess::Anyone);
	console.execute("GeometryTemplate.hasAdditiveAlpha 1", output, ConsoleAccess::Anyone);
	console.execute("GeometryTemplate.simpleUVTranslation -0.2/0", output, ConsoleAccess::Anyone);
	const geom::GeometryTemplate* rifle = geometries.find("eu_ar_rifle");
	CHECK(rifle && rifle->hasAdditiveAlpha && rifle->simpleUVTranslation[0] == -0.2f &&
		rifle->simpleUVTranslation[1] == 0);
	console.execute("GeometryTemplate.create BundledMesh plain", output, ConsoleAccess::Anyone);
	CHECK(!geometries.find("plain")->hasAdditiveAlpha);
}
