#include "GeometryTemplateManager.h"

#include "Console/Console.h"
#include "Console/ConsoleBinding.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace dice::hfe::geom {

namespace {

std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return s;
}

} // namespace

bool GeometryTemplateManager::Less::operator()(const std::string& a, const std::string& b) const
{
	return lower(a) < lower(b);
}

GeometryTemplate* GeometryTemplateManager::create(const std::string& type, const std::string& name,
	const std::string& scriptDirectory)
{
	const std::string kind = lower(type);
	const char* extension = nullptr;
	MeshKind meshKind = MeshKind::Static;
	if (kind == "staticmesh") {
		extension = ".staticMesh";
	} else if (kind == "bundledmesh") {
		extension = ".bundledMesh";
		meshKind = MeshKind::Bundled;
	} else if (kind == "skinnedmesh") {
		extension = ".skinnedMesh";
		meshKind = MeshKind::Skinned;
	} else if (kind == "meshparticlemesh") {
		// The meshes of MeshParticleSystems (debris, sparks, tracers): bundled
		// meshes in the files.
		extension = ".bundledMesh";
		meshKind = MeshKind::Bundled;
	} else if (kind == "roadcompiled") {
		meshKind = MeshKind::Road; // each road object loads its own mesh
	} else {
		m_active = nullptr;
		return nullptr;
	}
	auto& slot = m_templates[name];
	if (!slot) {
		slot = std::make_unique<GeometryTemplate>();
		slot->name = name;
		slot->kind = meshKind;
		slot->meshParticle = kind == "meshparticlemesh";
		// The templates' loads compose the same path: the script's folder,
		// "Meshes/", the name and the kind's extension (bundled 0x100a1860,
		// skinned 0x100d3080, MeshParticleMesh 0x100e9060, which takes a
		// "gpu_<name>" file instead when there is one; none ship).
		slot->path = extension ? scriptDirectory + "Meshes/" + name + extension
								: scriptDirectory + name + "_compiled.dat";
	}
	m_active = slot.get();
	return m_active;
}

GeometryTemplate* GeometryTemplateManager::find(const std::string& name) const
{
	const auto it = m_templates.find(name);
	return it == m_templates.end() ? nullptr : it->second.get();
}

void registerGeometryConsoleMethods(Console& console, GeometryTemplateManager& geometries)
{
	bindMethod(console, "GeometryTemplate.create", ConsoleAccess::Anyone, // 0x10072c14, 0x10072ca0, 0x10072d75
		[&console, &geometries](std::string type, std::string name) {
			const std::string& file = console.currentFile();
			const size_t slash = file.rfind('/');
			const std::string directory = slash == std::string::npos ? std::string() : file.substr(0, slash + 1);
			GeometryTemplate* created = geometries.create(type, name, directory);
			return created ? created->name : std::string();
		});
	bindMethod(console, "GeometryTemplate.hasAdditiveAlpha", ConsoleAccess::Anyone, [&geometries](bool on) { // 0x100740c3, 0x10074134, 0x10074181
		if (GeometryTemplate* active = geometries.active()) {
			active->hasAdditiveAlpha = on;
		}
	});
	bindMethod(console, "GeometryTemplate.useRadiusForShadowDepth", ConsoleAccess::Anyone, [&geometries](bool on) { // 0x10074932, 0x100749a3, 0x100749f0
		if (GeometryTemplate* active = geometries.active()) {
			active->useRadiusForShadowDepth = on;
		}
	});
	bindMethod(console, "GeometryTemplate.useRadiusForShadowSpatial", ConsoleAccess::Anyone, [&geometries](bool on) { // 0x10074a4b, 0x10074abc, 0x10074b09
		if (GeometryTemplate* active = geometries.active()) {
			active->useRadiusForShadowSpatial = on;
		}
	});
	bindMethod(console, "GeometryTemplate.simpleUVTranslation", ConsoleAccess::Anyone, [&geometries](std::string uv) { // 0x100741cb, 0x10074257, 0x100742c2
		if (GeometryTemplate* active = geometries.active()) {
			float u = 0, v = 0;
			std::sscanf(uv.c_str(), "%f/%f", &u, &v);
			active->simpleUVTranslation[0] = u;
			active->simpleUVTranslation[1] = v;
		}
	});
	bindMethod(console, "GeometryTemplate.maxSkip3pLods", ConsoleAccess::Anyone, [&geometries](unsigned lods) { // 0x100739ea, 0x10073a37
		if (GeometryTemplate* active = geometries.active()) {
			active->maxSkip3pLods = lods;
		}
	});
	bindMethod(console, "GeometryTemplate.maxSkipWreckLods", ConsoleAccess::Anyone, [&geometries](unsigned lods) { // 0x10073afb, 0x10073b54
		if (GeometryTemplate* active = geometries.active()) {
			active->maxSkipWreckLods = lods;
		}
	});
	// isTitanInterior / isTitanExterior exclude each other (0x10148690 / 0x10148670).
	bindProperty<bool>(console, "GeometryTemplate.isTitanInterior", ConsoleAccess::Anyone, // 0x100735f6, 0x10073643
		[&geometries] { return geometries.active() && geometries.active()->titanInterior; },
		[&geometries](const bool& on) {
			if (GeometryTemplate* active = geometries.active()) {
				active->titanInterior = on;
				if (on) {
					active->titanExterior = false;
				}
			}
		});
	bindProperty<bool>(console, "GeometryTemplate.isTitanExterior", ConsoleAccess::Anyone, // 0x10073483, 0x100734d0
		[&geometries] { return geometries.active() && geometries.active()->titanExterior; },
		[&geometries](const bool& on) {
			if (GeometryTemplate* active = geometries.active()) {
				active->titanExterior = on;
				if (on) {
					active->titanInterior = false;
				}
			}
		});
	bindProperty<int>(console, "GeometryTemplate.titanExteriorPartIndex", ConsoleAccess::Anyone, // 0x100743b2, 0x100743ff, 0x1007440b
		[&geometries] { return geometries.active() ? geometries.active()->titanExteriorPart : 0; },
		[&geometries](const int& part) {
			if (GeometryTemplate* active = geometries.active()) {
				active->titanExteriorPart = part;
			}
		});
	// The bundled templates' material maps (0x100a25d0 / 0x100a26b0); the
	// static ones' parallax (setMaterialParallaxHeightScale: bundled
	// templates ignore it, 0x100a7d70).
	const auto materialSetter = [&console, &geometries](const char* name, std::map<uint32_t, float> GeometryTemplate::*map,
									bool bundledOnly, bool staticOnly) {
		bindMethod(console, name, ConsoleAccess::Anyone,
			[&geometries, map, bundledOnly, staticOnly](int geometry, int lod, int material, float value) {
				GeometryTemplate* active = geometries.active();
				if (!active || (bundledOnly && active->kind != MeshKind::Bundled) ||
					(staticOnly && active->kind == MeshKind::Bundled)) {
					return;
				}
				(active->*map)[GeometryTemplate::materialKey(geometry, lod, material)] = value;
			});
	};
	materialSetter("GeometryTemplate.setMaterialReflectionScale", &GeometryTemplate::reflectionScale, true, false); // 0x10073d7a, 0x10073e71
	materialSetter("GeometryTemplate.setSpecularStaticGloss", &GeometryTemplate::staticGloss, true, false); // 0x10073f84, 0x1007407b
	materialSetter("GeometryTemplate.setMaterialParallaxHeightScale", &GeometryTemplate::parallaxHeightScale, false, true); // 0x100732d3, 0x100733ca
	bindProperty<int>(console, "GeometryTemplate.nrOfAnimatedUVMatrix", ConsoleAccess::Anyone, // 0x10073c18, 0x10073c65
		[&geometries] { return geometries.active() ? geometries.active()->animatedUVMatrices : 0; },
		[&geometries](const int& count) {
			GeometryTemplate* active = geometries.active();
			if (active && active->kind == MeshKind::Bundled) {
				active->animatedUVMatrices = count;
			}
		});
	bindMethod(console, "GeometryTemplate.setSubGeometryLodDistance", ConsoleAccess::Anyone, // 0x100730e9, 0x100731a8
		[&geometries](int geometry, int lod, float distance) {
			GeometryTemplate* active = geometries.active();
			if (!active || geometry < 0 || lod < 0 || geometry > 16 || lod > 16) {
				return;
			}
			auto& lods = active->lodDistances;
			if (lods.size() <= static_cast<size_t>(geometry)) {
				lods.resize(static_cast<size_t>(geometry) + 1);
			}
			auto& distances = lods[static_cast<size_t>(geometry)];
			if (distances.size() <= static_cast<size_t>(lod)) {
				distances.resize(static_cast<size_t>(lod) + 1, 0.0f);
			}
			distances[static_cast<size_t>(lod)] = distance;
		});
}

} // namespace dice::hfe::geom
