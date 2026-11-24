#pragma once

// Not retail: BF2142_FRAME_STATS=<seconds> logs the main thread's time per
// frame stage (mean, 95th percentile and max, ms) every <seconds> (5 if not a
// number), so a slow machine can say where its frame goes, and names the
// meshes and textures loaded in each frame over 50 ms. Unset, each scope
// costs one branch.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace dice::hfe::render {

class FrameStats {
public:
	// Frame is the time between two endFrame calls; Overlay .. Shadows run inside Draw;
	// Loads (first use of a mesh, texture, skeleton or animation) inside any of them.
	enum Stage { Frame, Events, Ticks, Sync, Acquire, Draw, Overlay, Menu, Particles, Prepare, Shadows, Submit, Loads,
		StageCount };

	explicit FrameStats(double intervalSeconds) : m_interval(static_cast<uint64_t>(intervalSeconds * 1e9)) {}

	static FrameStats& get()
	{
		static FrameStats stats([] {
			const char* value = std::getenv("BF2142_FRAME_STATS");
			const double seconds = value ? std::atof(value) : 0.0;
			return value ? (seconds > 0 ? seconds : 5.0) : 0.0; // set but not a number: every 5 s
		}());
		return stats;
	}

	static uint64_t now()
	{
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	bool enabled() const { return m_interval > 0; }
	void add(Stage stage, uint64_t ns) { m_current[stage] += ns; }
	// A load inside another (a mesh's textures) is named, not added again.
	void load(const std::string& what, uint64_t ns, bool nested = false)
	{
		if (!nested) {
			add(Loads, ns);
		}
		if (ns >= 1000000 && m_loads.size() < 4096) {
			m_loads.emplace_back(ns, what);
		}
	}

	// Closes a frame at `nowNs`; past the interval, returns the report (else "").
	std::string endFrame(uint64_t nowNs)
	{
		if (!enabled()) {
			return {};
		}
		static const char* const kNames[StageCount] = {"frame", "events", "ticks", "sync", "acquire", "draw", "overlay",
			"menu", "particles", "prepare", "shadows", "submit", "loads"};
		std::string hitch;
		if (m_last != 0) {
			m_current[Frame] = nowNs - m_last;
			for (int s = 0; s < StageCount; ++s) {
				m_samples[s].push_back(m_current[s]);
			}
			if (m_current[Frame] > kHitchNs) {
				hitch = hitchLine(kNames);
			}
		} else {
			m_windowStart = nowNs;
		}
		m_current = {};
		m_loads.clear();
		m_last = nowNs;
		if (nowNs - m_windowStart < m_interval || m_samples[Frame].empty()) {
			return hitch;
		}
		const size_t frames = m_samples[Frame].size();
		const double seconds = static_cast<double>(nowNs - m_windowStart) / 1e9;
		const size_t hitches = static_cast<size_t>(std::count_if(m_samples[Frame].begin(), m_samples[Frame].end(),
			[](uint64_t ns) { return ns > kHitchNs; }));
		char line[160];
		std::snprintf(line, sizeof line, "frame-stats: %.1f s, %zu frames, %.1f fps, %zu over 50 ms; ms mean/p95/max:", seconds,
			frames, static_cast<double>(frames) / seconds, hitches);
		std::string report = hitch.empty() ? line : hitch + "\n" + line;
		for (int s = 0; s < StageCount; ++s) {
			std::vector<uint64_t>& v = m_samples[s];
			double sum = 0;
			for (const uint64_t x : v) {
				sum += static_cast<double>(x);
			}
			const size_t at = std::min(v.size() - 1, v.size() * 95 / 100);
			const uint64_t most = *std::max_element(v.begin(), v.end());
			std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(at), v.end());
			std::snprintf(line, sizeof line, " %s %.2f/%.2f/%.1f", kNames[s], sum / static_cast<double>(v.size()) / 1e6,
				static_cast<double>(v[at]) / 1e6, static_cast<double>(most) / 1e6);
			report += line;
			v.clear();
		}
		m_windowStart = nowNs;
		return report;
	}

	// Times a first use of `what` (a mesh, texture, ...) as a load.
	class Load {
	public:
		explicit Load(const std::string& what) : m_what(what), m_start(get().enabled() ? now() : 0) { ++get().m_loadDepth; }
		~Load()
		{
			FrameStats& stats = get();
			--stats.m_loadDepth;
			if (m_start) {
				stats.load(m_what, now() - m_start, stats.m_loadDepth > 0);
			}
		}
		Load(const Load&) = delete;
		Load& operator=(const Load&) = delete;

	private:
		const std::string& m_what;
		uint64_t m_start;
	};

	// Adds the scope's time to `stage` when the stats are on.
	class Scope {
	public:
		explicit Scope(Stage stage) : m_stage(stage), m_start(get().enabled() ? now() : 0) {}
		~Scope()
		{
			if (m_start) {
				get().add(m_stage, now() - m_start);
			}
		}
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		Stage m_stage;
		uint64_t m_start;
	};

private:
	static constexpr uint64_t kHitchNs = 50000000;

	// "frame-hitch: <ms>; <stages over 1 ms>; slowest loads: <name ms>..."
	std::string hitchLine(const char* const* names)
	{
		char part[200];
		std::snprintf(part, sizeof part, "frame-hitch: %.1f ms;", static_cast<double>(m_current[Frame]) / 1e6);
		std::string line = part;
		for (int s = Frame + 1; s < StageCount; ++s) {
			if (m_current[s] >= 1000000) {
				std::snprintf(part, sizeof part, " %s %.1f", names[s], static_cast<double>(m_current[s]) / 1e6);
				line += part;
			}
		}
		std::sort(m_loads.begin(), m_loads.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		std::snprintf(part, sizeof part, "; %zu loads over 1 ms:", m_loads.size());
		line += part;
		for (size_t i = 0; i < m_loads.size() && i < 8; ++i) {
			std::snprintf(part, sizeof part, " %s %.1f,", m_loads[i].second.c_str(), static_cast<double>(m_loads[i].first) / 1e6);
			line += part;
		}
		if (line.back() == ',') {
			line.pop_back();
		}
		return line;
	}

	uint64_t m_interval;
	uint64_t m_last = 0;
	uint64_t m_windowStart = 0;
	std::array<uint64_t, StageCount> m_current {};
	std::array<std::vector<uint64_t>, StageCount> m_samples;
	std::vector<std::pair<uint64_t, std::string>> m_loads; // this frame's loads over 1 ms
	int m_loadDepth = 0;
};

} // namespace dice::hfe::render
