#pragma once

// Not retail: BF2142_FRAME_STATS=<seconds> logs the main thread's time per
// frame stage (mean, 95th percentile and max, ms) every <seconds> (5 if not a
// number), so a slow machine can say where its frame goes. Unset, empty, 0 or
// negative, it's off and each scope costs one branch.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace dice::hfe::render {

class FrameStats {
public:
	// Frame is the time between two endFrame calls; Overlay .. Shadows run inside Draw.
	enum Stage { Frame, Events, Ticks, Sync, Acquire, Draw, Overlay, Menu, Particles, Prepare, Shadows, Submit, StageCount };

	explicit FrameStats(double intervalSeconds) : m_interval(static_cast<uint64_t>(intervalSeconds * 1e9)) {}

	static FrameStats& get()
	{
		static FrameStats stats(intervalFrom(std::getenv("BF2142_FRAME_STATS")));
		return stats;
	}

	// The variable's value as an interval in seconds: unset, empty, 0 or negative
	// is off (0); a positive number is itself; anything else is every 5 s.
	static double intervalFrom(const char* value)
	{
		if (!value || !*value) {
			return 0.0;
		}
		char* end = nullptr;
		const double seconds = std::strtod(value, &end);
		if (end == value) {
			return 5.0; // set but not a number
		}
		return seconds > 0 ? seconds : 0.0;
	}

	static uint64_t now()
	{
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	bool enabled() const { return m_interval > 0; }
	void add(Stage stage, uint64_t ns) { m_current[stage] += ns; }

	// Drops the frame in progress (no swapchain image, nothing drawn): its stage
	// times go nowhere, and the next frame is timed from `nowNs`.
	void dropFrame(uint64_t nowNs)
	{
		if (!enabled()) {
			return;
		}
		m_current = {};
		if (m_last != 0) {
			m_last = nowNs;
		}
	}

	// Closes a frame at `nowNs`; past the interval, returns the report (else "").
	std::string endFrame(uint64_t nowNs)
	{
		if (!enabled()) {
			return {};
		}
		if (m_last != 0) {
			m_current[Frame] = nowNs - m_last;
			for (int s = 0; s < StageCount; ++s) {
				m_samples[s].push_back(m_current[s]);
			}
		} else {
			m_windowStart = nowNs;
		}
		m_current = {};
		m_last = nowNs;
		if (nowNs - m_windowStart < m_interval || m_samples[Frame].empty()) {
			return {};
		}
		static const char* const kNames[StageCount] = {"frame", "events", "ticks", "sync", "acquire", "draw", "overlay",
			"menu", "particles", "prepare", "shadows", "submit"};
		const size_t frames = m_samples[Frame].size();
		const double seconds = static_cast<double>(nowNs - m_windowStart) / 1e9;
		const size_t hitches = static_cast<size_t>(std::count_if(m_samples[Frame].begin(), m_samples[Frame].end(),
			[](uint64_t ns) { return ns > 50000000; }));
		char line[160];
		std::snprintf(line, sizeof line, "frame-stats: %.1f s, %zu frames, %.1f fps, %zu over 50 ms; ms mean/p95/max:", seconds,
			frames, static_cast<double>(frames) / seconds, hitches);
		std::string report = line;
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
	uint64_t m_interval;
	uint64_t m_last = 0;
	uint64_t m_windowStart = 0;
	std::array<uint64_t, StageCount> m_current {};
	std::array<std::vector<uint64_t>, StageCount> m_samples;
};

} // namespace dice::hfe::render
