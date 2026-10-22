#include "VfsSoundFileSystem.h"

#include "IO/File/FileManager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace dice::hfe::sound {

namespace {

// Files are read whole; sound files are small or streamed in chunks anyway.
struct OpenFile {
	std::vector<uint8_t> data;
	size_t position = 0;
};

} // namespace

void* VfsSoundFileSystem::open(const char* path)
{
	auto file = std::make_unique<OpenFile>();
	if (!path || !m_files.readFile(path, file->data)) {
		return nullptr;
	}
	return file.release();
}

void VfsSoundFileSystem::close(void* file)
{
	delete static_cast<OpenFile*>(file);
}

long VfsSoundFileSystem::seek(void* handle, long offset, int origin)
{
	auto* file = static_cast<OpenFile*>(handle);
	if (!file) {
		return -1;
	}
	long base = 0;
	if (origin == SEEK_CUR) {
		base = static_cast<long>(file->position);
	} else if (origin == SEEK_END) {
		base = static_cast<long>(file->data.size());
	}
	const long target = std::clamp(base + offset, 0L, static_cast<long>(file->data.size()));
	file->position = static_cast<size_t>(target);
	return target;
}

size_t VfsSoundFileSystem::read(void* handle, void* buffer, size_t bytes)
{
	auto* file = static_cast<OpenFile*>(handle);
	if (!file) {
		return 0;
	}
	const size_t count = std::min(bytes, file->data.size() - file->position);
	std::memcpy(buffer, file->data.data() + file->position, count);
	file->position += count;
	return count;
}

} // namespace dice::hfe::sound
