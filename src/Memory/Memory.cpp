#include "Memory.h"

#include "ChunkPools.h"
#include "Platform/Platform.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace dice::hfe::memory {

namespace {

// Every block starts with a header just before the pointer handed out.
// In 32-bit builds (the retail layout) it is one 32-bit word:
//   pooled:   bit 1 set, bits 2-16 block size, bits 17-31 pool index
//   unpooled: bit 1 clear, bits 2-31 block size
// The block size includes the header itself.
//
// The chunk pools assume 32-bit pointers (a 4-byte chunk holds a free-list
// link) and only give 4-byte alignment, so 64-bit builds skip them and use a
// header padded to the platform's malloc alignment instead.
constexpr bool kUseChunkPools = sizeof(void*) == 4;
constexpr size_t kHeaderSize = kUseChunkPools ? sizeof(uintptr_t) : alignof(std::max_align_t);
constexpr uintptr_t kPooledFlag = 0x2;

// Requests above this are almost certainly corrupt sizes.
constexpr unsigned int kSaneAllocationLimit = 0x6000000;

constexpr unsigned int kDefaultMaxAllocSize = 0x1000000;
constexpr const char* kStatus = "Memory.dll Ver:1.01 Date:2005/01/10 - Final";
constexpr const char* kNotInFinalBuild = "no active in final build";

ChunkPools* g_pools = nullptr;
std::thread::id g_mainThread;
unsigned int g_maxAllocSize = kDefaultMaxAllocSize;

// Set after a fatal error. From then on everything bypasses the pools, so the
// error path can't recurse into a broken allocator.
bool g_failed = false;

uintptr_t pooledHeader(size_t blockSize, int poolIndex)
{
	return (static_cast<uintptr_t>(poolIndex) << 17) | ((blockSize & 0x7fff) << 2) | kPooledFlag;
}

uintptr_t unpooledHeader(size_t blockSize)
{
	return static_cast<uintptr_t>(blockSize) << 2;
}

bool isPooled(uintptr_t header) { return header & kPooledFlag; }
size_t pooledBlockSize(uintptr_t header) { return (header >> 2) & 0x7fff; }
int pooledIndex(uintptr_t header) { return static_cast<int>(header >> 17); }
size_t unpooledBlockSize(uintptr_t header) { return header >> 2; }

uintptr_t& headerOf(void* block)
{
	return *reinterpret_cast<uintptr_t*>(static_cast<std::byte*>(block) - kHeaderSize);
}

void* blockAfter(void* header)
{
	return static_cast<std::byte*>(header) + kHeaderSize;
}

void reportError(bool recoverable, const char* message)
{
	if (!recoverable) {
		g_failed = true;
	}
	platform::showErrorDialog("BF2142 Memory Error", message);
	if (!recoverable) {
		std::abort();
	}
}

} // namespace

void initialize()
{
	if (kUseChunkPools && !g_pools) {
		g_pools = new ChunkPools();
	}
	g_mainThread = std::this_thread::get_id();
}

void shutdown()
{
	// Outstanding pooled blocks become invalid; only call this at process exit.
	delete g_pools;
	g_pools = nullptr;
}

void* bf_malloc(unsigned int size, int /*memoryNameId*/)
{
	if (g_failed) {
		return std::malloc(size);
	}
	if (size == 0) {
		size = 1;
	}
	if (size > kSaneAllocationLimit) {
		char message[128];
		std::snprintf(message, sizeof(message),
			"memory.dll: sanity check: block size %u (%5.2f MB) doesn't seem sane", size,
			size / (1024.0 * 1024.0));
		reportError(true, message);
	}

	const size_t blockSize = size + kHeaderSize;
	if (g_pools && std::this_thread::get_id() == g_mainThread) {
		ChunkPools::Allocation pooled = g_pools->allocate(blockSize);
		if (pooled.chunk) {
			void* block = blockAfter(pooled.chunk);
			headerOf(block) = pooledHeader(blockSize, pooled.poolIndex);
			return block;
		}
	}

	void* header = std::malloc(blockSize);
	if (!header) {
		char message[128];
		std::snprintf(message, sizeof(message), "memory.dll: all alloc attempts failed for size %u", size);
		reportError(false, message);
		return nullptr;
	}
	void* block = blockAfter(header);
	headerOf(block) = unpooledHeader(blockSize);
	return block;
}

void bf_free(void* block)
{
	if (g_failed || !block) {
		return;
	}
	const uintptr_t header = headerOf(block);
	void* start = static_cast<std::byte*>(block) - kHeaderSize;
	if (isPooled(header)) {
		// Pools are not thread-safe; only the main thread may own pooled blocks.
		if (std::this_thread::get_id() != g_mainThread) {
			reportError(false, "memory.dll: another thread is deallocating main thread memory");
		}
		if (g_pools && g_pools->free(start, pooledBlockSize(header), pooledIndex(header))) {
			return;
		}
		char message[128];
		std::snprintf(message, sizeof(message), "memory.dll: ChunkPools::free fails at memory %p",
			start);
		reportError(false, message);
	}
	std::free(start);
}

int getSizeOfAllocation(void* block)
{
	if (!block) {
		return 0;
	}
	const uintptr_t header = headerOf(block);
	const size_t blockSize = isPooled(header) ? pooledBlockSize(header) : unpooledBlockSize(header);
	return static_cast<int>(blockSize - kHeaderSize);
}

int addMemoryName(const char*) { return 0; }

const char* getStatus(const char*) { return kStatus; }

// The retail build ignores the callback; errors always go to a message box.
void register_err_callback(ErrorCallback) {}

void set_max_alloc_size(unsigned int size) { g_maxAllocSize = size; }
unsigned int get_max_alloc_size() { return g_maxAllocSize; }
void reset_max_alloc_size() { g_maxAllocSize = kDefaultMaxAllocSize; }

int get_total_bytes_allocated() { return 0; }
int64_t get_accumulated_bytes_allocated() { return 0; }
int get_total_allocation_call_count() { return 0; }
int get_accumulated_allocation_call_count() { return 0; }
void nextVisualFrame() {}
void nextPhysicFrame() {}
void memory_update_modules(const char*) {}
void set_all_allocations_to_zero() {}
const char* dump_allocations_to_file(const char*, int) { return kNotInFinalBuild; }
const char* dump_active_allocations_to_file(const char*) { return kNotInFinalBuild; }
const char* memoryTraceReport() { return "use_traceIdentifiers not enabled"; }
void memoryTraceReportToFile(const char*) {}
void startMemoryTrace(int&, const char*) {}
void endMemoryTrace(int) {}
void memoryTraceReset() {}
void resourceTraceAllocate(int&, const char*, int) {}
void resourceTraceFree(int&, const char*, int) {}

void get_callstack_debug_str(char* buffer, int, int)
{
	static constexpr char kMessage[] = "Only available in DEBUG builds";
	std::snprintf(buffer, sizeof(kMessage), "%s", kMessage);
}

unsigned int histogramCurrentUsedMemory() { return 0; }
unsigned int histogramPeakUsedMemory() { return 0; }
void histogramDumpToFile(const char*) {}
void histogramClearPeaks() {}
void histogramClearCurrent() {}

} // namespace dice::hfe::memory
