// Piecewise-linear curve used by sound templates for volume and pitch, e.g.
// volume as a function of engine RPM. Each control point has a y range; for
// randomised envelopes 64 variants with y picked inside that range are rolled
// once at construction, and a sound selects one with its random seed so it
// keeps a consistent but individual character.
#pragma once

#include <vector>

namespace dice::hfe::sound {

// One-slot interface the game may also implement for custom curves.
class ISoundEnvelope {
public:
	virtual float evaluate(float x, unsigned seed) = 0;

protected:
	~ISoundEnvelope() = default;
};

struct EnvelopePoint {
	float x;
	float yLow;
	float yHigh;
};

class SoundEnvelope final : public ISoundEnvelope {
public:
	static constexpr unsigned VariantCount = 64;

	SoundEnvelope(float minX, float maxX, float minY, float maxY, bool randomized,
		const EnvelopePoint* points, int count);
	// Evaluates the curve at x for the variant chosen by `seed`.
	float evaluate(float x, unsigned seed) override;

	const std::vector<EnvelopePoint>& points() const { return m_points; }
	float minX() const { return m_minX; }
	float maxX() const { return m_maxX; }
	float minY() const { return m_minY; }
	float maxY() const { return m_maxY; }
	bool randomized() const { return m_randomized; }

private:
	float m_minX;
	float m_maxX;
	float m_minY;
	float m_maxY;
	bool m_randomized;
	std::vector<EnvelopePoint> m_points;
	std::vector<float> m_values; // variant-major: m_values[variant * points + i]
};

} // namespace dice::hfe::sound
