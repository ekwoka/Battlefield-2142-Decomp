// BF2142_FRAME_STATS (src/Render/FrameStats.h): the frame is the time between
// endFrame calls, each stage's mean and 95th percentile over the window.
#include "Render/FrameStats.h"
#include "TestMain.h"

#include <string>

using dice::hfe::render::FrameStats;

TEST(FrameStats_ReportsMeanAndP95PerWindow)
{
	FrameStats off(0);
	CHECK(!off.enabled());
	CHECK(off.endFrame(1000).empty());

	FrameStats stats(1.0); // a 1 s window
	CHECK(stats.enabled());
	const uint64_t ms = 1000000;
	uint64_t t = 1000 * ms;
	CHECK(stats.endFrame(t).empty()); // opens the window, no frame yet
	// 100 frames of 10 ms: 95 with a 2 ms tick stage, 5 with 40 ms.
	for (int i = 0; i < 100; ++i) {
		stats.add(FrameStats::Ticks, i < 95 ? 2 * ms : 40 * ms);
		t += 10 * ms;
		const std::string report = stats.endFrame(t);
		CHECK(report.empty() == (i < 99));
		if (!report.empty()) {
			CHECK(report.find("100 frames") != std::string::npos);
			CHECK(report.find("100.0 fps") != std::string::npos);
			CHECK(report.find(" frame 10.00/10.00") != std::string::npos);
			// Mean (95*2 + 5*40)/100 = 3.9; the 95th of 100 is the first 40 ms one.
			CHECK(report.find(" ticks 3.90/40.00") != std::string::npos);
			CHECK(report.find(" sync 0.00/0.00") != std::string::npos);
		}
	}
	// The next window starts empty.
	t += 10 * ms;
	CHECK(stats.endFrame(t).empty());
}
