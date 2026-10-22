#include "Memory/Memory.h"
#include "TestMain.h"

#include <cstring>
#include <thread>
#include <vector>

namespace mem = dice::hfe::memory;

TEST(Memory_PooledAndLargeBlocksRoundTrip)
{
	mem::initialize();
	for (unsigned size : {0u, 1u, 3u, 4u, 44u, 84u, 85u, 200u, 1u << 20}) {
		auto* p = static_cast<unsigned char*>(mem::bf_malloc(size, 0));
		CHECK(p != nullptr);
		CHECK(mem::getSizeOfAllocation(p) >= static_cast<int>(size == 0 ? 1 : size));
		std::memset(p, 0xab, size);
		mem::bf_free(p);
	}
}

TEST(Memory_ManySmallAllocationsSpanPools)
{
	mem::initialize();
	// 16-byte blocks (20 with header) -> 52428 per 1 MiB slab; force several slabs.
	std::vector<void*> blocks;
	for (int i = 0; i < 200000; ++i) {
		void* p = mem::bf_malloc(16, 0);
		CHECK(p != nullptr);
		*static_cast<int*>(p) = i;
		blocks.push_back(p);
	}
	for (int i = 0; i < 200000; ++i) {
		CHECK(*static_cast<int*>(blocks[i]) == i);
	}
	// Free every other block, then reallocate: freed chunks must be reused.
	for (size_t i = 0; i < blocks.size(); i += 2) {
		mem::bf_free(blocks[i]);
	}
	for (size_t i = 0; i < blocks.size(); i += 2) {
		blocks[i] = mem::bf_malloc(16, 0);
		CHECK(blocks[i] != nullptr);
	}
	for (void* p : blocks) {
		mem::bf_free(p);
	}
}

TEST(Memory_OtherThreadsBypassPools)
{
	mem::initialize();
	void* p = nullptr;
	std::thread([&] { p = mem::bf_malloc(8, 0); }).join();
	CHECK(p != nullptr);
	CHECK(mem::getSizeOfAllocation(p) == 8);
	mem::bf_free(p); // unpooled, so freeing from the main thread is fine
}

TEST(Memory_StatusStrings)
{
	CHECK(std::strcmp(mem::getStatus(nullptr), "Memory.dll Ver:1.01 Date:2005/01/10 - Final") == 0);
	CHECK(mem::get_max_alloc_size() == 0x1000000);
	mem::set_max_alloc_size(5);
	CHECK(mem::get_max_alloc_size() == 5);
	mem::reset_max_alloc_size();
	CHECK(mem::get_max_alloc_size() == 0x1000000);
}
