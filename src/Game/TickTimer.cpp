#include "TickTimer.h"

#include <algorithm>

namespace dice::hfe {

int TickTimer::advance(double now)
{
	double elapsed = now - m_lastTick;
	if (elapsed < m_tickLength) {
		return 0;
	}
	int ticks = 0;
	while (elapsed - m_tickLength >= 0 && ticks <= m_maxTicks) {
		elapsed -= m_tickLength;
		++ticks;
	}
	if (ticks > m_maxTicks) {
		// Too far behind: drop the backlog and resynchronise.
		m_lastTick = m_previousTick = m_nextTick = now;
		return m_maxTicks;
	}
	m_previousTick = m_nextTick;
	// The original accumulates in float precision.
	const float last = static_cast<float>(ticks) * m_tickLength + static_cast<float>(m_lastTick);
	m_lastTick = last;
	m_nextTick = last + m_tickLength;
	return ticks;
}

double TickTimer::interpolation(double now) const
{
	return std::clamp((now - m_lastTick) / m_tickLength, 0.0, 1.0);
}

} // namespace dice::hfe
