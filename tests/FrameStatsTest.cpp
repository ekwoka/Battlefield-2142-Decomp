// BF2142_FRAME_STATS (src/Render/FrameStats.h): the frame is the time between
// endFrame calls, each stage's mean, 95th percentile and max over the window,
// and the frames over 50 ms.
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
		CHECK(report.empty() == (i < 99));
		if (!report.empty()) {
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
