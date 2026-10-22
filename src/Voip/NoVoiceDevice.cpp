// Builds without SDL3 have no microphone or speakers for voice: the voice
// client and server still run (and the tests use a memory device).
#include "VoiceChat.h"

namespace dice::hfe::voip {

std::unique_ptr<VoiceDevice> openVoiceDevice(std::string* error)
{
	if (error) {
		*error = "built without SDL3 audio";
	}
	return nullptr;
}

bool voiceDeviceAvailable()
{
	return false;
}

} // namespace dice::hfe::voip
