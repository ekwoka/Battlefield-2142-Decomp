// A per-tick profiler for finding hitches, off unless BF2142_TICK_PROF is
// set (to the milliseconds over which a tick is printed with its timers).
// TP("name") times the rest of its scope into a named slot; the game loop
// brackets each tick with tickprof::beginTick / endTick and prints
// tickprof::report at the end of a --simulate run.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace dice::hfe::tickprof {

constexpr int kSlots = 128;

struct State {
	const char* names[kSlots] = {"total"};
	uint64_t acc[kSlots] = {};
	uint32_t calls[kSlots] = {};
	double sums[kSlots] = {};
	double maxes[kSlots] = {};
	std::vector<double> totals;
	int slots = 1;
	double threshold = 25;
	bool on = false;
	std::thread::id main;
	State()
	{
		if (const char* env = std::getenv("BF2142_TICK_PROF")) {
			on = true;
			threshold = std::atof(env);
		}
		main = std::this_thread::get_id();
	}
};

inline State& state()
{
	static State s;
	return s;
}

inline bool enabled()
{
	return state().on;
}

inline int named(const char* name)
{
	State& s = state();
	if (s.slots >= kSlots) {
		return kSlots - 1;
	}
	s.names[s.slots] = name;
	return s.slots++;
}

inline uint64_t now()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Times its scope (on the main thread only: workers' time is not the tick's).
struct Scope {
	int slot;
	uint64_t start = 0;
	explicit Scope(int s) : slot(s)
	{
		if (state().on && std::this_thread::get_id() == state().main) {
			start = now();
		}
	}
	~Scope()
	{
		if (start) {
			state().acc[slot] += now() - start;
			++state().calls[slot];
		}
	}
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
};

inline void beginTick()
{
	State& s = state();
	if (!s.on) {
		return;
	}
	std::fill(std::begin(s.acc), std::end(s.acc), 0);
	std::fill(std::begin(s.calls), std::end(s.calls), 0);
}

inline void endTick(long tick)
{
	State& s = state();
	if (!s.on) {
		return;
	}
	const double ms = s.acc[0] / 1e6;
	s.totals.push_back(ms);
	for (int i = 0; i < s.slots; ++i) {
		s.sums[i] += s.acc[i] / 1e6;
		s.maxes[i] = std::max(s.maxes[i], s.acc[i] / 1e6);
	}
	if (ms >= s.threshold) {
		std::printf("tickprof: tick %ld (%.1f s) %.1f ms:", tick, tick / 30.0, ms);
		for (int i = 1; i < s.slots; ++i) {
			if (s.acc[i] >= 500000) {
				std::printf(" %s=%.1f(%u)", s.names[i], s.acc[i] / 1e6, s.calls[i]);
			}
		}
		std::printf("\n");
	}
}

inline void report()
{
	State& s = state();
	if (!s.on || s.totals.empty()) {
		return;
	}
	std::vector<double> v = s.totals;
	const size_t n = v.size();
	std::sort(v.begin(), v.end());
	double sum = 0;
	for (double x : v) {
		sum += x;
	}
	// Without tick 0 (the round's first step, part of the load).
	std::vector<double> rest(s.totals.begin() + 1, s.totals.end());
	std::sort(rest.begin(), rest.end());
	std::printf("tickprof: %zu ticks mean %.2f p50 %.2f p90 %.2f p99 %.2f p99.9 %.2f max %.1f ms (after tick 0: max %.1f, p99 %.2f); "
				">33ms %zu, >50ms %zu, >100ms %zu\n",
		n, sum / n, v[n / 2], v[n * 9 / 10], v[n * 99 / 100], v[std::min(n - 1, n * 999 / 1000)], v[n - 1],
		rest.empty() ? 0.0 : rest.back(), rest.empty() ? 0.0 : rest[rest.size() * 99 / 100],
		static_cast<size_t>(std::count_if(v.begin(), v.end(), [](double x) { return x > 33.3; })),
		static_cast<size_t>(std::count_if(v.begin(), v.end(), [](double x) { return x > 50; })),
		static_cast<size_t>(std::count_if(v.begin(), v.end(), [](double x) { return x > 100; })));
	for (int i = 1; i < s.slots; ++i) {
		if (s.sums[i] > 0) {
			std::printf("tickprof:   %-24s mean %.3f max %.1f ms\n", s.names[i], s.sums[i] / n, s.maxes[i]);
		}
	}
}

} // namespace dice::hfe::tickprof

#define BF2142_TP_CAT2(a, b) a##b
#define BF2142_TP_CAT(a, b) BF2142_TP_CAT2(a, b)
#define TP(name) \
	static const int BF2142_TP_CAT(tpSlot, __LINE__) = ::dice::hfe::tickprof::named(name); \
	::dice::hfe::tickprof::Scope BF2142_TP_CAT(tpScope, __LINE__)(BF2142_TP_CAT(tpSlot, __LINE__))
