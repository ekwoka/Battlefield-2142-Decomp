#include "VoipCodec.h"

#include <speex/speex.h>

#include <algorithm>
#include <cmath>

namespace dice::hfe::voip {

namespace {

SpeexBits* bitsOf(void* bits)
{
	return static_cast<SpeexBits*>(bits);
}

int16_t clampSample(float value)
{
	return static_cast<int16_t>(std::lround(std::clamp(value, -32768.0f, 32767.0f)));
}

} // namespace

SpeexEncoder::SpeexEncoder(int speexQuality) : m_quality(std::clamp(speexQuality, 0, 10))
{
	m_state = speex_encoder_init(&speex_nb_mode);
	m_bits = new SpeexBits;
	speex_bits_init(bitsOf(m_bits));
	speex_encoder_ctl(m_state, SPEEX_SET_QUALITY, &m_quality);
	spx_int32_t bitrate = 0;
	speex_encoder_ctl(m_state, SPEEX_GET_BITRATE, &bitrate);
	m_frameBytes = static_cast<size_t>(bitrate / 50 / 8);
}

SpeexEncoder::~SpeexEncoder()
{
	speex_encoder_destroy(m_state);
	speex_bits_destroy(bitsOf(m_bits));
	delete bitsOf(m_bits);
}

void SpeexEncoder::encode(const int16_t* samples, std::vector<uint8_t>& out)
{
	// 0x00737c70: the samples as floats, one frame, its bytes.
	float input[kFrameSamples];
	for (int i = 0; i < kFrameSamples; ++i) {
		input[i] = samples[i];
	}
	speex_bits_reset(bitsOf(m_bits));
	speex_encode(m_state, input, bitsOf(m_bits));
	std::vector<char> bytes(std::max<size_t>(m_frameBytes, 1) + 16);
	const int written = speex_bits_write(bitsOf(m_bits), bytes.data(), static_cast<int>(m_frameBytes));
	const size_t at = out.size();
	out.resize(at + m_frameBytes, 0);
	std::copy(bytes.begin(), bytes.begin() + std::min<size_t>(static_cast<size_t>(std::max(written, 0)), m_frameBytes),
		out.begin() + static_cast<std::ptrdiff_t>(at));
}

SpeexDecoder::SpeexDecoder()
{
	// 0x00737c20: perceptual enhancement on.
	m_state = speex_decoder_init(&speex_nb_mode);
	int enhance = 1;
	speex_decoder_ctl(m_state, SPEEX_SET_ENH, &enhance);
	m_bits = new SpeexBits;
	speex_bits_init(bitsOf(m_bits));
}

SpeexDecoder::~SpeexDecoder()
{
	speex_decoder_destroy(m_state);
	speex_bits_destroy(bitsOf(m_bits));
	delete bitsOf(m_bits);
}

bool SpeexDecoder::decode(const uint8_t* frame, size_t bytes, int16_t* samples)
{
	float output[kFrameSamples] = {};
	speex_bits_read_from(bitsOf(m_bits), reinterpret_cast<const char*>(frame), static_cast<int>(bytes));
	const int result = speex_decode(m_state, bitsOf(m_bits), output);
	for (int i = 0; i < kFrameSamples; ++i) {
		samples[i] = result == 0 ? clampSample(output[i]) : 0;
	}
	return result == 0;
}

void SpeexDecoder::conceal(int16_t* samples)
{
	float output[kFrameSamples] = {};
	speex_decode(m_state, nullptr, output);
	for (int i = 0; i < kFrameSamples; ++i) {
		samples[i] = clampSample(output[i]);
	}
}

size_t speexFrameBytes(int speexQuality)
{
	SpeexEncoder encoder(speexQuality);
	return encoder.frameBytes();
}

} // namespace dice::hfe::voip
