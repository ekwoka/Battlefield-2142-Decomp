#include "SoundEnvelope.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace dice::hfe::sound {

SoundEnvelope::SoundEnvelope(float minX, float maxX, float minY, float maxY, bool randomized,
	const EnvelopePoint* points, int count)
	: m_minX(minX), m_maxX(maxX), m_minY(minY), m_maxY(maxY), m_randomized(randomized),
	  m_points(points, points + count)
{
	if (m_maxX < m_minX) {
		std::swap(m_minX, m_maxX);
	}
	if (m_maxY < m_minY) {
		std::swap(m_minY, m_maxY);
	}
	for (EnvelopePoint& p : m_points) {
		p.x = std::clamp(p.x, m_minX, m_maxX);
		p.yLow = std::clamp(p.yLow, m_minY, m_maxY);
		p.yHigh = std::clamp(p.yHigh, m_minY, m_maxY);
	}
	std::sort(m_points.begin(), m_points.end(),
		[](const EnvelopePoint& a, const EnvelopePoint& b) { return a.x < b.x; });

	// Variant 0 is the low curve; the others are random within each point's range.
	const unsigned variants = m_randomized ? VariantCount : 1;
	const size_t n = m_points.size();
	m_values.resize(n * variants);
	for (size_t i = 0; i < n; ++i) {
		m_values[i] = m_points[i].yLow;
	}
	for (unsigned v = 1; v < variants; ++v) {
		for (size_t i = 0; i < n; ++i) {
			const float t = static_cast<float>(std::rand()) / 32767.0f;
			m_values[v * n + i] = m_points[i].yLow + (m_points[i].yHigh - m_points[i].yLow) * t;
		}
	}
}

float SoundEnvelope::evaluate(float x, unsigned seed)
{
	const size_t n = m_points.size();
	if (n == 0) {
		return m_maxY;
	}
	const unsigned variant = m_randomized ? (seed & (VariantCount - 1)) : 0;
	const float* values = &m_values[variant * n];

	if (x <= m_points.front().x) {
		return values[0];
	}
	if (x >= m_points.back().x) {
		return values[n - 1];
	}
	for (size_t i = 1; i < n; ++i) {
		if (x < m_points[i].x) {
			const float dx = m_points[i].x - m_points[i - 1].x;
			if (std::fabs(dx) <= FLT_EPSILON) {
				return values[i];
			}
			return values[i - 1] + (x - m_points[i - 1].x) / dx * (values[i] - values[i - 1]);
		}
	}
	return values[0];
}

} // namespace dice::hfe::sound
