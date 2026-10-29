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
	bindMethod(console, "GeometryTemplate.create", ConsoleAccess::Anyone,
		[&console, &geometries](std::string type, std::string name) {
			const std::string& file = console.currentFile();
			const size_t slash = file.rfind('/');
			const std::string directory = slash == std::string::npos ? std::string() : file.substr(0, slash + 1);
			GeometryTemplate* created = geometries.create(type, name, directory);
			return created ? created->name : std::string();
		});
	bindMethod(console, "GeometryTemplate.hasAdditiveAlpha", ConsoleAccess::Anyone, [&geometries](bool on) {
		if (GeometryTemplate* active = geometries.active()) {
			active->hasAdditiveAlpha = on;
		}
	});
	bindMethod(console, "GeometryTemplate.useRadiusForShadowDepth", ConsoleAccess::Anyone, [&geometries](bool on) {
		if (GeometryTemplate* active = geometries.active()) {
			active->useRadiusForShadowDepth = on;
		}
	});
	bindMethod(console, "GeometryTemplate.useRadiusForShadowSpatial", ConsoleAccess::Anyone, [&geometries](bool on) {
		if (GeometryTemplate* active = geometries.active()) {
			active->useRadiusForShadowSpatial = on;
		}
	});
	bindMethod(console, "GeometryTemplate.simpleUVTranslation", ConsoleAccess::Anyone, [&geometries](std::string uv) {
		if (GeometryTemplate* active = geometries.active()) {
			float u = 0, v = 0;
			std::sscanf(uv.c_str(), "%f/%f", &u, &v);
			active->simpleUVTranslation[0] = u;
			active->simpleUVTranslation[1] = v;
		}
	});
	bindMethod(console, "GeometryTemplate.maxSkip3pLods", ConsoleAccess::Anyone, [&geometries](unsigned lods) {
		if (GeometryTemplate* active = geometries.active()) {
			active->maxSkip3pLods = lods;
		}
	});
	bindMethod(console, "GeometryTemplate.maxSkipWreckLods", ConsoleAccess::Anyone, [&geometries](unsigned lods) {
		if (GeometryTemplate* active = geometries.active()) {
			active->maxSkipWreckLods = lods;
		}
	});
	// isTitanInterior / isTitanExterior exclude each other (0x10148690 / 0x10148670).
	bindProperty<bool>(console, "GeometryTemplate.isTitanInterior", ConsoleAccess::Anyone,
		[&geometries] { return geometries.active() && geometries.active()->titanInterior; },
		[&geometries](const bool& on) {
			if (GeometryTemplate* active = geometries.active()) {
				active->titanInterior = on;
				if (on) {
					active->titanExterior = false;
				}
			}
		});
	bindProperty<bool>(console, "GeometryTemplate.isTitanExterior", ConsoleAccess::Anyone,
		[&geometries] { return geometries.active() && geometries.active()->titanExterior; },
		[&geometries](const bool& on) {
			if (GeometryTemplate* active = geometries.active()) {
				active->titanExterior = on;
				if (on) {
					active->titanInterior = false;
				}
			}
		});
	bindProperty<int>(console, "GeometryTemplate.titanExteriorPartIndex", ConsoleAccess::Anyone,
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
	materialSetter("GeometryTemplate.setMaterialReflectionScale", &GeometryTemplate::reflectionScale, true, false);
	materialSetter("GeometryTemplate.setSpecularStaticGloss", &GeometryTemplate::staticGloss, true, false);
	materialSetter("GeometryTemplate.setMaterialParallaxHeightScale", &GeometryTemplate::parallaxHeightScale, false, true);
	bindProperty<int>(console, "GeometryTemplate.nrOfAnimatedUVMatrix", ConsoleAccess::Anyone,
		[&geometries] { return geometries.active() ? geometries.active()->animatedUVMatrices : 0; },
		[&geometries](const int& count) {
			GeometryTemplate* active = geometries.active();
			if (active && active->kind == MeshKind::Bundled) {
				active->animatedUVMatrices = count;
			}
		});
	bindMethod(console, "GeometryTemplate.setSubGeometryLodDistance", ConsoleAccess::Anyone,
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
