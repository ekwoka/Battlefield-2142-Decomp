// dice::hfe::memory - the engine-wide allocator (originally Memory.dll,
// "Memory.dll Ver:1.01 Date:2005/01/10 - Final").
//
// Every DICE module routes its allocations through bf_malloc/bf_free. The
// retail build only implements the allocator itself; the tracing, histogram
// and statistics entry points are stubs that report "not active in final
// build". They are kept so callers compile unchanged.
#pragma once

#include <cstdint>

namespace dice::hfe::memory {

using ErrorCallback = void (*)(bool recoverable, const char* message);

void* bf_malloc(unsigned int size, int memoryNameId);
void bf_free(void* block);
int getSizeOfAllocation(void* block);

// Memory names group allocations for tracing. Tracing is compiled out of the
// final build, so every name maps to id 0.
int addMemoryName(const char* name);

const char* getStatus(const char* unused);
void register_err_callback(ErrorCallback callback);

void set_max_alloc_size(unsigned int size);
unsigned int get_max_alloc_size();
void reset_max_alloc_size();

// Statistics and tracing: inactive in the final build.
int get_total_bytes_allocated();
int64_t get_accumulated_bytes_allocated();
int get_total_allocation_call_count();
int get_accumulated_allocation_call_count();
void nextVisualFrame();
void nextPhysicFrame();
void memory_update_modules(const char* moduleList);
void set_all_allocations_to_zero();
const char* dump_allocations_to_file(const char* path, int flags);
const char* dump_active_allocations_to_file(const char* path);
const char* memoryTraceReport();
void memoryTraceReportToFile(const char* path);
void startMemoryTrace(int& traceId, const char* name);
void endMemoryTrace(int traceId);
void memoryTraceReset();
void resourceTraceAllocate(int& traceId, const char* name, int size);
void resourceTraceFree(int& traceId, const char* name, int size);
void get_callstack_debug_str(char* buffer, int skipFrames, int maxFrames);

// Large-allocation histogram. Nothing records into it in the final build.
unsigned int histogramCurrentUsedMemory();
unsigned int histogramPeakUsedMemory();
void histogramDumpToFile(const char* path);
void histogramClearPeaks();
void histogramClearCurrent();

// Sets up the chunk pools and records the calling thread as the main thread.
// The retail DLL did this from DllMain(DLL_PROCESS_ATTACH).
void initialize();
void shutdown();

} // namespace dice::hfe::memory
