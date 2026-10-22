// Lets the sound engine read files through the game's virtual file system
// (archives included), as the executable's sound module does.
#pragma once

#include "Sound/SoundEngine/SoundServices.h"

namespace dice::hfe::io {
class FileManager;
}

namespace dice::hfe::sound {

class VfsSoundFileSystem final : public ISoundFileSystem {
public:
	explicit VfsSoundFileSystem(const io::FileManager& files) : m_files(files) {}

	void* open(const char* path) override;
	void close(void* file) override;
	long seek(void* file, long offset, int origin) override;
	size_t read(void* file, void* buffer, size_t bytes) override;

private:
	const io::FileManager& m_files;
};

} // namespace dice::hfe::sound
