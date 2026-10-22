// RIFF/WAVE reader (originally WavFileFormat.cpp).
//
// Besides the samples, BF2142 stores loop points in the file: a single cue
// point marks the loop start and either a playlist (plst) entry or a labelled
// text region (LIST/adtl/ltxt) gives the loop length. Sounds may therefore
// have a one-shot start block, a looping middle and a one-shot tail.
#pragma once

#include <cstdint>
#include <vector>

namespace dice::hfe::sound {

struct WavData {
	uint16_t formatTag = 0;      // 1 = PCM
	uint16_t channels = 0;
	uint32_t sampleRate = 0;
	uint16_t blockAlign = 0;     // bytes per sample frame
	uint16_t bitsPerSample = 0;
	std::vector<uint8_t> samples;
	uint32_t frames = 0;

	// Loop region in frames. Without loop data the whole file is the "loop".
	int32_t loopStart = 0;
	int32_t loopEnd = 0;
	bool hasLoop = false;
};

// Reads `path` through the sound file system. Returns false if the file is
// missing or not a WAVE file; corrupt optional chunks only log a warning.
bool readWavFile(const char* path, WavData& out);

} // namespace dice::hfe::sound
