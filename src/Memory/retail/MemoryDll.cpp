// Drop-in replacement for the retail Memory.dll. Each export forwards to
// dice::hfe::memory; Memory.def gives them their original MSVC-mangled names.
// MSVC passes references as pointers, hence the int* parameters.
#include "Memory/Memory.h"
#include "RetailCompat/Export.h"

#include <windows.h>

namespace mem = dice::hfe::memory;

RETAIL_EXPORT int Memory_addMemoryName(const char* a) { return mem::addMemoryName(a); }
RETAIL_EXPORT void Memory_bf_free(void* a) { mem::bf_free(a); }
RETAIL_EXPORT void* Memory_bf_malloc(unsigned int a, int b) { return mem::bf_malloc(a, b); }
RETAIL_EXPORT const char* Memory_dump_active_allocations_to_file(const char* a) { return mem::dump_active_allocations_to_file(a); }
RETAIL_EXPORT const char* Memory_dump_allocations_to_file(const char* a, int b) { return mem::dump_allocations_to_file(a, b); }
RETAIL_EXPORT void Memory_endMemoryTrace(int a) { mem::endMemoryTrace(a); }
RETAIL_EXPORT int Memory_getSizeOfAllocation(void* a) { return mem::getSizeOfAllocation(a); }
RETAIL_EXPORT const char* Memory_getStatus(const char* a) { return mem::getStatus(a); }
RETAIL_EXPORT int Memory_get_accumulated_allocation_call_count() { return mem::get_accumulated_allocation_call_count(); }
RETAIL_EXPORT int64_t Memory_get_accumulated_bytes_allocated() { return mem::get_accumulated_bytes_allocated(); }
RETAIL_EXPORT void Memory_get_callstack_debug_str(char* a, int b, int c) { mem::get_callstack_debug_str(a, b, c); }
RETAIL_EXPORT unsigned int Memory_get_max_alloc_size() { return mem::get_max_alloc_size(); }
RETAIL_EXPORT int Memory_get_total_allocation_call_count() { return mem::get_total_allocation_call_count(); }
RETAIL_EXPORT int Memory_get_total_bytes_allocated() { return mem::get_total_bytes_allocated(); }
RETAIL_EXPORT void Memory_histogramClearCurrent() { mem::histogramClearCurrent(); }
RETAIL_EXPORT void Memory_histogramClearPeaks() { mem::histogramClearPeaks(); }
RETAIL_EXPORT unsigned int Memory_histogramCurrentUsedMemory() { return mem::histogramCurrentUsedMemory(); }
RETAIL_EXPORT void Memory_histogramDumpToFile(const char* a) { mem::histogramDumpToFile(a); }
RETAIL_EXPORT unsigned int Memory_histogramPeakUsedMemory() { return mem::histogramPeakUsedMemory(); }
RETAIL_EXPORT const char* Memory_memoryTraceReport() { return mem::memoryTraceReport(); }
RETAIL_EXPORT void Memory_memoryTraceReportToFile(const char* a) { mem::memoryTraceReportToFile(a); }
RETAIL_EXPORT void Memory_memoryTraceReset() { mem::memoryTraceReset(); }
RETAIL_EXPORT void Memory_memory_update_modules(const char* a) { mem::memory_update_modules(a); }
RETAIL_EXPORT void Memory_nextPhysicFrame() { mem::nextPhysicFrame(); }
RETAIL_EXPORT void Memory_nextVisualFrame() { mem::nextVisualFrame(); }
RETAIL_EXPORT void Memory_register_err_callback(mem::ErrorCallback a) { mem::register_err_callback(a); }
RETAIL_EXPORT void Memory_reset_max_alloc_size() { mem::reset_max_alloc_size(); }
RETAIL_EXPORT void Memory_resourceTraceAllocate(int* a, const char* b, int c) { mem::resourceTraceAllocate(*a, b, c); }
RETAIL_EXPORT void Memory_resourceTraceFree(int* a, const char* b, int c) { mem::resourceTraceFree(*a, b, c); }
RETAIL_EXPORT void Memory_set_all_allocations_to_zero() { mem::set_all_allocations_to_zero(); }
RETAIL_EXPORT void Memory_set_max_alloc_size(unsigned int a) { mem::set_max_alloc_size(a); }
RETAIL_EXPORT void Memory_startMemoryTrace(int* a, const char* b) { mem::startMemoryTrace(*a, b); }

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		mem::initialize();
	}
	return TRUE;
}
