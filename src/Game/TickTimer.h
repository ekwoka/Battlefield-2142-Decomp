// The fixed simulation clock (0x00677f50 / 0x00677f90).
//
// The game simulates in fixed ticks: 30 per second, at most 10 per frame
// (BF2Engine creates it as TickTimer(10, 30.0)). Each frame asks how many
// whole ticks have passed since the last one. When the game falls more than
// the maximum behind, the clock jumps to the present instead of catching up.
#pragma once

namespace dice::hfe {

class TickTimer {
public:
	TickTimer(int maxTicksPerFrame, float ticksPerSecond)
		: m_tickLength(1.0f / ticksPerSecond), m_maxTicks(maxTicksPerFrame)
	{
	}

	float tickLength() const { return m_tickLength; }

	// Ticks to simulate at time `now` (seconds).
	int advance(double now);

	// Time of the last simulated tick, and when the next one is due.
	double lastTickTime() const { return m_lastTick; }
	double nextTickTime() const { return m_nextTick; }
	// How far `now` is between the last tick and the next (0..1), for
	// interpolating what is drawn.
	double interpolation(double now) const;

private:
	float m_tickLength;
	int m_maxTicks;
	double m_lastTick = 0;
	double m_previousTick = 0;
	double m_nextTick = 0;
};

} // namespace dice::hfe
