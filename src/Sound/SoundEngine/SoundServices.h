// Services the sound engine needs from its host (originally BF2142Audio.dll's
// logger and file-system hooks). The game implements both interfaces, so their
// method order is part of the engine's binary interface. The game replaces both through
// ISoundEngine::setFileSystem / setLogger so that sounds load from the mod's
// zip archives and messages go to the game log. Without a host, the defaults
// below use stdio.
#pragma once

#include <cstddef>
#include <cstdio>

namespace dice::hfe::sound {

class ISoundLogger {
public:
	virtual void error(const char* message, const char* file, int line) = 0;
	virtual void warning(const char* message, const char* file, int line) = 0;
	virtual void info(const char* message, const char* file, int line) = 0;
	virtual void assertionFailed(const char* message, const char* file, int line) = 0;
	virtual void debug(const char* message, const char* file, int line) = 0;

protected:
	~ISoundLogger() = default;
};

// Files are opaque handles; the default implementation uses FILE*.
class ISoundFileSystem {
public:
	virtual void* open(const char* path) = 0;
	virtual void close(void* file) = 0;
	// Returns the new position, so seek(file, 0, SEEK_END) yields the size.
	virtual long seek(void* file, long offset, int origin) = 0;
	virtual size_t read(void* file, void* buffer, size_t bytes) = 0;

protected:
	~ISoundFileSystem() = default;
};

ISoundLogger& soundLog();
ISoundFileSystem& soundFiles();

// Passing nullptr restores the built-in default.
void setSoundLogger(ISoundLogger* logger);
void setSoundFileSystem(ISoundFileSystem* fileSystem);

// True when $BF2142_SOUND_LOG tracing is on (enables periodic statistics).
bool soundTraceEnabled();

// printf-style formatting into a shared buffer, as the original did for every
// log call. Not thread-safe; the music thread formats its own messages.
const char* formatSoundMessage(const char* format, ...);

} // namespace dice::hfe::sound

#define SND_ERROR(...) ::dice::hfe::sound::soundLog().error(::dice::hfe::sound::formatSoundMessage(__VA_ARGS__), __FILE__, __LINE__)
#define SND_WARNING(...) ::dice::hfe::sound::soundLog().warning(::dice::hfe::sound::formatSoundMessage(__VA_ARGS__), __FILE__, __LINE__)
#define SND_INFO(...) ::dice::hfe::sound::soundLog().info(::dice::hfe::sound::formatSoundMessage(__VA_ARGS__), __FILE__, __LINE__)
#define SND_DEBUG(...) ::dice::hfe::sound::soundLog().debug(::dice::hfe::sound::formatSoundMessage(__VA_ARGS__), __FILE__, __LINE__)
