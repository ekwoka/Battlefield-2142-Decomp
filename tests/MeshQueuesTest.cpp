// Which render queue a material takes and the transparency queues' order
// (RendDX9 0x100c3880, 0x10039380; src/Render/MeshQueues.h).
#include "Render/MeshQueues.h"
#include "Render/Culling.h"
#include "Geometry/GeometryTemplateManager.h"
#include "Console/Console.h"
#include "TestMain.h"

#include <cmath>
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

TEST(MeshQueues_GeometryTemplatesLodDistancesAndMeshParticles)
{
	Console console;
	geom::GeometryTemplateManager geometries;
	geom::registerGeometryConsoleMethods(console, geometries);
	std::string output;
	console.execute("GeometryTemplate.create StaticMesh house", output, ConsoleAccess::Anyone);
	console.execute("GeometryTemplate.setSubGeometryLodDistance 0 0 75", output, ConsoleAccess::Anyone);
	console.execute("GeometryTemplate.setSubGeometryLodDistance 0 1 150", output, ConsoleAccess::Anyone);
	console.execute("GeometryTemplate.setSubGeometryLodDistance 0 2 400", output, ConsoleAccess::Anyone);
	const geom::GeometryTemplate* house = geometries.find("house");
	CHECK(house && house->lodDistances.size() == 1 && house->lodDistances[0].size() == 3);
	CHECK(house->lodDistances[0][0] == 75 && house->lodDistances[0][2] == 400);
	// LOD i is drawn up to its distance, the last beyond.
	const std::vector<float>& d = house->lodDistances[0];
	CHECK(render::lodForDistance(10, d, 4) == 0 && render::lodForDistance(76, d, 4) == 1);
	CHECK(render::lodForDistance(300, d, 4) == 2 && render::lodForDistance(5000, d, 4) == 3);
	CHECK(render::lodForDistance(5000, d, 2) == 1 && render::lodForDistance(5000, d, 1) == 0);
	CHECK(render::lodForDistance(5000, d, 4, 0) == 3 && render::lodForDistance(76, d, 4, 2) == 0);
	// Unset distances: 50 m, then 50 m more each (0x100d4d30); a set one
	// moves the later defaults on by itself.
	const std::vector<float> defaults = render::lodSwitchDistances({}, 4);
	CHECK(defaults.size() == 3 && defaults[0] == 50 && defaults[1] == 100 && defaults[2] == 150);
	const std::vector<float> mixed = render::lodSwitchDistances({30}, 3);
	CHECK(mixed.size() == 2 && mixed[0] == 30 && mixed[1] == 80);
	CHECK(render::lodDistance(120, 2, 10) == 50);
	// MeshParticleMesh templates are bundled-mesh files.
	console.execute("GeometryTemplate.create MeshParticleMesh mp_spark2", output, ConsoleAccess::Anyone);
	const geom::GeometryTemplate* spark = geometries.find("mp_spark2");
	CHECK(spark && spark->meshParticle && spark->kind == geom::MeshKind::Bundled);
	CHECK(spark->path.size() > 12 && spark->path.compare(spark->path.size() - 12, 12, ".bundledMesh") == 0);
}

TEST(Culling_FrustumSpheresAndBoxes)
{
	// Looking along +z from the origin, 90 degrees, near 1, far 100.
	const render::Mat4 viewProjection = render::Mat4::lookAt({0, 0, 0}, {0, 0, 1}, {0, 1, 0}) *
		render::Mat4::perspective(1.5707963f, 1.0f, 1.0f, 100.0f);
	const render::Frustum frustum = render::Frustum::fromViewProjection(viewProjection);
	CHECK(frustum.containsSphere({0, 0, 10}, 1));
	CHECK(!frustum.containsSphere({0, 0, -10}, 1));        // behind
	CHECK(!frustum.containsSphere({0, 0, 150}, 1));        // beyond the far plane
	CHECK(!frustum.containsSphere({30, 0, 10}, 1));        // off to the right
	CHECK(frustum.containsSphere({30, 0, 10}, 25));        // but reaching in
	CHECK(frustum.containsSphere({0, 0, -10}, -1));        // unknown bounds: kept
	CHECK(frustum.containsBox({-1, -1, 5}, {1, 1, 6}));
	CHECK(!frustum.containsBox({-1, -1, -6}, {1, 1, -5}));
	CHECK(frustum.containsBox({-500, -10, -500}, {500, 10, 500})); // around the camera
	// Spheres: around points, moved and scaled.
	const render::BoundingSphere s = render::sphereAround({{-1, 0, 0}, {3, 0, 0}, {1, 2, 0}});
	CHECK(s.center.x == 1 && s.center.y == 1 && std::fabs(s.radius - std::sqrt(5.0f)) < 1e-5f);
	render::Mat4 world = render::Mat4::translation({10, 0, 0});
	world.at(0, 0) = world.at(1, 1) = world.at(2, 2) = 2;
	const render::BoundingSphere moved = render::transformSphere(s, world);
	CHECK(moved.center.x == 12 && moved.center.y == 2 && std::fabs(moved.radius - 2 * std::sqrt(5.0f)) < 1e-4f);
}
