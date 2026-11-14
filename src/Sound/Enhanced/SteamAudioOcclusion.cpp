#include "SteamAudioOcclusion.h"

#include "Platform/Platform.h"
#include "Sound/SoundEngine/SoundServices.h"

#include <phonon.h>

#include <chrono>

namespace dice::hfe::sound {

namespace {

#define PHONON_FUNCTIONS(X)                                                                                         \
	X(iplContextCreate) X(iplContextRelease) X(iplSceneCreate) X(iplSceneRelease) X(iplSceneCommit)                 \
	X(iplStaticMeshCreate) X(iplStaticMeshAdd) X(iplStaticMeshRelease) X(iplSimulatorCreate) X(iplSimulatorRelease) \
	X(iplSimulatorSetScene) X(iplSimulatorCommit) X(iplSimulatorSetSharedInputs) X(iplSimulatorRunDirect)           \
	X(iplSourceCreate) X(iplSourceRelease) X(iplSourceAdd) X(iplSourceSetInputs) X(iplSourceGetOutputs)

struct Phonon {
#define PHONON_SLOT(f) decltype(&::f) f = nullptr;
	PHONON_FUNCTIONS(PHONON_SLOT)
#undef PHONON_SLOT
};

const Phonon* phonon()
{
	static Phonon api;
	static const bool loaded = [] {
		void* library = platform::loadLibrary(BF2142_PHONON_LIBRARY);
		if (!library) {
			SND_WARNING("Enhanced audio: could not load %s", BF2142_PHONON_LIBRARY);
			return false;
		}
		bool complete = true;
#define PHONON_BIND(f)                                                                     \
	api.f = reinterpret_cast<decltype(api.f)>(platform::findSymbol(library, #f)); \
	complete = complete && api.f;
		PHONON_FUNCTIONS(PHONON_BIND)
#undef PHONON_BIND
		return complete;
	}();
	return loaded ? &api : nullptr;
}

// Steam Audio is right-handed; the game is left-handed (OpenALSample flips z too).
IPLVector3 toIpl(const Vec3& v) { return {v.x, v.y, -v.z}; }

// One material for every face: Steam Audio's "generic" preset.
constexpr IPLMaterial kMaterial {{0.10f, 0.20f, 0.30f}, 0.05f, {0.100f, 0.050f, 0.030f}};
constexpr int kOcclusionSamples = 16;   // volumetric: smooth edges
constexpr float kOcclusionRadius = 1.0f; // metres
constexpr int kTransmissionRays = 3;
constexpr int kMaxSources = 256;
// Steam Audio's direct path has no diffraction: a corner or an overhang next
// to the listener costs -30 dB (two faces at 0.1 transmission) where retail's
// obstruction (SoundManager::obstruction) costs about -2 dB. So a blocked
// sound keeps at least kMinGain and kMinGainHF of its highs, the rest scaled.
constexpr float kMinGain = 0.5f;
constexpr float kMinGainHF = 0.5f;

} // namespace

struct SteamAudioOcclusion::Impl {
	const Phonon* api = nullptr;
	IPLContext context = nullptr;
	IPLScene scene = nullptr;
	IPLStaticMesh mesh = nullptr;
	IPLSimulator simulator = nullptr;
	std::vector<IPLSource> sources;

	~Impl()
	{
		if (!api) {
			return;
		}
		for (IPLSource& source : sources) {
			api->iplSourceRelease(&source);
		}
		if (simulator) {
			api->iplSimulatorRelease(&simulator);
		}
		if (mesh) {
			api->iplStaticMeshRelease(&mesh);
		}
		if (scene) {
			api->iplSceneRelease(&scene);
		}
		if (context) {
			api->iplContextRelease(&context);
		}
	}
};

SteamAudioOcclusion::SteamAudioOcclusion() = default;

SteamAudioOcclusion::~SteamAudioOcclusion()
{
	if (enhancedOcclusion() == this) {
		setEnhancedOcclusion(nullptr);
	}
}

bool SteamAudioOcclusion::build(const std::vector<Vec3>& corners)
{
	const auto started = std::chrono::steady_clock::now();
	m_impl.reset();
	const Phonon* api = phonon();
	if (!api || corners.size() < 3) {
		return false;
	}
	auto impl = std::make_unique<Impl>();
	impl->api = api;

	IPLContextSettings contextSettings {};
	contextSettings.version = STEAMAUDIO_VERSION;
	if (api->iplContextCreate(&contextSettings, &impl->context) != IPL_STATUS_SUCCESS) {
		return false;
	}
	IPLSceneSettings sceneSettings {};
	sceneSettings.type = IPL_SCENETYPE_DEFAULT;
	if (api->iplSceneCreate(impl->context, &sceneSettings, &impl->scene) != IPL_STATUS_SUCCESS) {
		return false;
	}

	const size_t triangles = corners.size() / 3;
	std::vector<IPLVector3> vertices;
	std::vector<IPLTriangle> faces;
	vertices.reserve(triangles * 3);
	faces.reserve(triangles);
	for (size_t i = 0; i < triangles * 3; i += 3) {
		const IPLint32 at = static_cast<IPLint32>(vertices.size());
		vertices.push_back(toIpl(corners[i]));
		vertices.push_back(toIpl(corners[i + 1]));
		vertices.push_back(toIpl(corners[i + 2]));
		faces.push_back({{at, at + 2, at + 1}}); // the mirror reverses the winding
	}
	std::vector<IPLint32> materialIndices(faces.size(), 0);
	IPLMaterial material = kMaterial;
	IPLStaticMeshSettings meshSettings {};
	meshSettings.numVertices = static_cast<IPLint32>(vertices.size());
	meshSettings.numTriangles = static_cast<IPLint32>(faces.size());
	meshSettings.numMaterials = 1;
	meshSettings.vertices = vertices.data();
	meshSettings.triangles = faces.data();
	meshSettings.materialIndices = materialIndices.data();
	meshSettings.materials = &material;
	if (api->iplStaticMeshCreate(impl->scene, &meshSettings, &impl->mesh) != IPL_STATUS_SUCCESS) {
		return false;
	}
	api->iplStaticMeshAdd(impl->mesh, impl->scene);
	api->iplSceneCommit(impl->scene);

	IPLSimulationSettings simulationSettings {};
	simulationSettings.flags = IPL_SIMULATIONFLAGS_DIRECT;
	simulationSettings.sceneType = IPL_SCENETYPE_DEFAULT;
	simulationSettings.reflectionType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
	simulationSettings.maxNumOcclusionSamples = kOcclusionSamples;
	simulationSettings.maxNumSources = kMaxSources;
	simulationSettings.numThreads = 1;
	simulationSettings.samplingRate = 44100;
	simulationSettings.frameSize = 1024;
	if (api->iplSimulatorCreate(impl->context, &simulationSettings, &impl->simulator) != IPL_STATUS_SUCCESS) {
		return false;
	}
	api->iplSimulatorSetScene(impl->simulator, impl->scene);
	api->iplSimulatorCommit(impl->simulator);

	m_impl = std::move(impl);
	m_stats = {};
	m_stats.triangles = triangles;
	m_stats.buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	SND_INFO("Enhanced audio: Steam Audio scene of %zu triangles in %.0f ms", triangles, m_stats.buildSeconds * 1000.0);
	return true;
}

void SteamAudioOcclusion::simulate(const Vec3& listener, std::vector<EnhancedSource>& sources)
{
	if (!m_impl) {
		return;
	}
	const auto started = std::chrono::steady_clock::now();
	const Phonon& api = *m_impl->api;
	std::vector<IPLSource>& pool = m_impl->sources;
	const size_t count = std::min(sources.size(), static_cast<size_t>(kMaxSources));
	if (pool.size() < count) {
		while (pool.size() < count) {
			IPLSourceSettings settings {IPL_SIMULATIONFLAGS_DIRECT};
			IPLSource source = nullptr;
			if (api.iplSourceCreate(m_impl->simulator, &settings, &source) != IPL_STATUS_SUCCESS) {
				break;
			}
			api.iplSourceAdd(source, m_impl->simulator);
			pool.push_back(source);
		}
		api.iplSimulatorCommit(m_impl->simulator);
	}

	const IPLCoordinateSpace3 axes {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}, toIpl(listener)};
	IPLSimulationSharedInputs shared {};
	shared.listener = axes;
	api.iplSimulatorSetSharedInputs(m_impl->simulator, IPL_SIMULATIONFLAGS_DIRECT, &shared);
	for (size_t i = 0; i < pool.size(); ++i) {
		IPLSimulationInputs inputs {};
		inputs.flags = IPL_SIMULATIONFLAGS_DIRECT;
		inputs.source = axes;
		if (i < count) { // the pool's spare sources simulate nothing
			inputs.directFlags =
				static_cast<IPLDirectSimulationFlags>(IPL_DIRECTSIMULATIONFLAGS_OCCLUSION | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
			inputs.source.origin = toIpl(sources[i].position);
			inputs.occlusionType = IPL_OCCLUSIONTYPE_VOLUMETRIC;
			inputs.occlusionRadius = kOcclusionRadius;
			inputs.numOcclusionSamples = kOcclusionSamples;
			inputs.numTransmissionRays = kTransmissionRays;
		}
		api.iplSourceSetInputs(pool[i], IPL_SIMULATIONFLAGS_DIRECT, &inputs);
	}
	api.iplSimulatorRunDirect(m_impl->simulator);

	const size_t done = std::min(count, pool.size());
	for (size_t i = 0; i < done; ++i) {
		IPLSimulationOutputs outputs {};
		api.iplSourceGetOutputs(pool[i], IPL_SIMULATIONFLAGS_DIRECT, &outputs);
		// What the direct effect does per band: the clear part plus the
		// transmitted part of the occluded rest.
		const float occlusion = outputs.direct.occlusion;
		const float low = occlusion + (1.0f - occlusion) * outputs.direct.transmission[0];
		const float high = occlusion + (1.0f - occlusion) * outputs.direct.transmission[2];
		m_stats.muffled += low < 0.5f ? 1 : 0;
		const float ratio = low > 0.0f ? std::min(1.0f, high / low) : 0.0f;
		sources[i].gain = kMinGain + (1.0f - kMinGain) * low;
		sources[i].gainHF = kMinGainHF + (1.0f - kMinGainHF) * ratio;
	}
	m_stats.simulateSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	++m_stats.ticks;
	m_stats.sources += static_cast<long>(done);
}

} // namespace dice::hfe::sound
