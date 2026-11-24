// BF2142_FRAME_STATS (src/Render/FrameStats.h): the frame is the time between
// endFrame calls, each stage's mean, 95th percentile and max over the window,
// and the frames over 50 ms, each named with its loads.
#include "Render/FrameStats.h"
#include "TestMain.h"

#include <string>

using dice::hfe::render::FrameStats;

TEST(FrameStats_ReportsMeanAndP95PerWindow)
{
	FrameStats off(0);
	CHECK(!off.enabled());
	CHECK(off.endFrame(1000).empty());

	FrameStats stats(1.09); // the 100 frames below take 1.09 s
	CHECK(stats.enabled());
	const uint64_t ms = 1000000;
	uint64_t t = 1000 * ms;
	CHECK(stats.endFrame(t).empty()); // opens the window, no frame yet
	// 100 frames: 95 with a 2 ms tick stage, 5 with 40 ms; frame 10 ms except one of 60 and one of 50.
	for (int i = 0; i < 100; ++i) {
		stats.add(FrameStats::Ticks, i < 95 ? 2 * ms : 40 * ms);
		t += i == 10 ? 60 * ms : i == 11 ? 50 * ms : 10 * ms;
		const std::string report = stats.endFrame(t);
		CHECK(report.empty() == (i < 99 && i != 10));
		if (i == 10) {
			CHECK(report == "frame-hitch: 60.0 ms; ticks 2.0; 0 loads over 1 ms:"); // over 50 ms: logged at once
		} else if (!report.empty()) {
			CHECK(report.find("100 frames") != std::string::npos);
			CHECK(report.find("1 over 50 ms") != std::string::npos); // 50 ms itself is not over
			CHECK(report.find(" frame 10.90/10.00/60.0") != std::string::npos); // (98*10 + 60 + 50)/100
			// Mean (95*2 + 5*40)/100 = 3.9; the 95th of 100 is the first 40 ms one.
			CHECK(report.find(" ticks 3.90/40.00/40.0") != std::string::npos);
			CHECK(report.find(" sync 0.00/0.00/0.0") != std::string::npos);
		}
	}
	// The next window starts empty.
	t += 10 * ms;
	CHECK(stats.endFrame(t).empty());
}

TEST(FrameStats_NamesTheLoadsOfAFrameOver50ms)
{
	FrameStats stats(100); // no window report in this test
	const uint64_t ms = 1000000;
	uint64_t t = 1000 * ms;
	stats.endFrame(t);
	stats.load("small.dds", ms / 2); // under 1 ms: timed, not named
	stats.load("tank.bundledmesh", 30 * ms);
	stats.load("tank.dds", 12 * ms);
	stats.add(FrameStats::Sync, 45 * ms);
	t += 20 * ms;
	CHECK(stats.endFrame(t).empty()); // a 20 ms frame is no hitch
	stats.load("rock.staticmesh", 40 * ms);
	stats.load("rock.dds", 30 * ms, true); // inside the mesh's load: named, not added
	stats.add(FrameStats::Sync, 41 * ms);
	t += 60 * ms;
	const std::string hitch = stats.endFrame(t);
	// Only this frame's loads; the earlier frame's were dropped.
	CHECK(hitch == "frame-hitch: 60.0 ms; sync 41.0 loads 40.0; 2 loads over 1 ms: rock.staticmesh 40.0, rock.dds 30.0");
}
