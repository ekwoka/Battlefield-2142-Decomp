#include "SoundServices.h"

#include <cstdarg>
#include <cstdlib>

namespace dice::hfe::sound {

namespace {

class StdioLogger final : public ISoundLogger {
public:
	void error(const char* message, const char*, int) override { print("SND-ERR: ", message); }
	void warning(const char* message, const char*, int) override { print("SND-WRN: ", message); }
	void info(const char* message, const char*, int) override { print("SND-INF: ", message); }
	void debug(const char* message, const char*, int) override { print("SND-DBG: ", message); }

	void assertionFailed(const char* message, const char*, int) override
	{
		print("SND-ASSERTION FAILED: ", message);
		std::abort();
	}

private:
	static void print(const char* prefix, const char* message) { std::fprintf(stderr, "%s%s\n", prefix, message); }
};

class StdioFileSystem final : public ISoundFileSystem {
public:
	void* open(const char* path) override { return std::fopen(path, "rb"); }
	void close(void* file) override { std::fclose(static_cast<FILE*>(file)); }
	size_t read(void* file, void* buffer, size_t bytes) override
	{
		return std::fread(buffer, 1, bytes, static_cast<FILE*>(file));
	}
	long seek(void* file, long offset, int origin) override
	{
		std::fseek(static_cast<FILE*>(file), offset, origin);
		return std::ftell(static_cast<FILE*>(file));
	}
};

StdioLogger g_defaultLogger;
StdioFileSystem g_defaultFileSystem;

// Copies every message to $BF2142_SOUND_LOG when set, whoever the host logger
// is; useful when the game swallows the engine's output.
class TraceLogger final : public ISoundLogger {
public:
	void error(const char* message, const char* file, int line) override { trace("ERR", message); host()->error(message, file, line); }
	void warning(const char* message, const char* file, int line) override { trace("WRN", message); host()->warning(message, file, line); }
	void info(const char* message, const char* file, int line) override { trace("INF", message); host()->info(message, file, line); }
	void debug(const char* message, const char* file, int line) override { trace("DBG", message); host()->debug(message, file, line); }
	void assertionFailed(const char* message, const char* file, int line) override
	{
		trace("ASSERT", message);
		host()->assertionFailed(message, file, line);
	}

	ISoundLogger* hostLogger = &g_defaultLogger;

private:
	ISoundLogger* host() { return hostLogger; }
	static void trace(const char* level, const char* message)
	{
		static FILE* file = [] {
			const char* path = std::getenv("BF2142_SOUND_LOG");
			return path ? std::fopen(path, "a") : nullptr;
		}();
		if (file) {
			std::fprintf(file, "SND-%s: %s\n", level, message);
			std::fflush(file);
		}
	}
};
TraceLogger g_traceLogger;

ISoundLogger* g_logger = &g_traceLogger;
ISoundFileSystem* g_fileSystem = &g_defaultFileSystem;

} // namespace

ISoundLogger& soundLog() { return *g_logger; }
ISoundFileSystem& soundFiles() { return *g_fileSystem; }

void setSoundLogger(ISoundLogger* logger) { g_traceLogger.hostLogger = logger ? logger : &g_defaultLogger; }
void setSoundFileSystem(ISoundFileSystem* fileSystem) { g_fileSystem = fileSystem ? fileSystem : &g_defaultFileSystem; }

bool soundTraceEnabled()
{
	static const bool enabled = std::getenv("BF2142_SOUND_LOG") != nullptr;
	return enabled;
}

const char* formatSoundMessage(const char* format, ...)
{
	static char buffer[0x800];
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

} // namespace dice::hfe::sound
