#include "Game/TickTimer.h"
#include "TestMain.h"

TEST(TickTimer_CountsWholeTicks)
{
	dice::hfe::TickTimer timer(10, 30.0f);
	CHECK(timer.advance(0.01) == 0);
	CHECK(timer.advance(0.034) == 1);    // one tick of 1/30 s
	CHECK(timer.advance(0.05) == 0);     // the remainder carries over
	CHECK(timer.advance(0.101) == 2);    // 0.0333 .. 0.1 (exactly 0.1 is a hair short in float)
	CHECK(timer.lastTickTime() > 0.0999 && timer.lastTickTime() < 0.1001);
}

TEST(TickTimer_ResynchronisesWhenFarBehind)
{
	dice::hfe::TickTimer timer(10, 30.0f);
	CHECK(timer.advance(5.0) == 10); // 150 ticks behind: capped, clock jumps to now
	CHECK(timer.lastTickTime() == 5.0);
	CHECK(timer.advance(5.04) == 1);
}
