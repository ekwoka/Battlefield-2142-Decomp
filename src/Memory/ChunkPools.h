// Small-block allocator used by bf_malloc on the main thread.
//
// Requests of up to MaxPooledSize bytes (header included) are rounded up to
// one of a fixed set of size classes. Each size class owns a chain of pools;
// a pool carves one 1 MiB slab into equal chunks and keeps a free list. Pools
// are created on demand and never released until shutdown.
//
// Each class remembers the pools that still have free chunks ("partially
// free" list) so allocation doesn't have to walk full pools.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dice::hfe::memory {

class ChunkPools {
public:
	static constexpr size_t SlabSize = 0x100000;
	static constexpr int MaxPoolsPerClass = 512;

	struct Allocation {
		void* chunk = nullptr;
		int poolIndex = 0;
	};

	ChunkPools();
	~ChunkPools();

	size_t maxPooledSize() const { return m_maxPooledSize; }

	// Returns a chunk of at least `size` bytes, or a null chunk if the size
	// isn't pooled or every pool in its class is exhausted.
	Allocation allocate(size_t size);

	// Returns a chunk to the pool it came from. `size` and `poolIndex` must be
	// the values that allocate() was given and returned.
	bool free(void* chunk, size_t size, int poolIndex);

private:
	struct Pool {
		int index = 0;         // position in its class, stored in block headers
		size_t chunkSize = 0;
		int capacity = 0;      // chunks per slab
		int used = 0;
		std::unique_ptr<std::byte[]> slab; // allocated on first use
		void* freeList = nullptr;
		bool onPartialList = false;
		Pool* nextPartial = nullptr;
	};

	struct SizeClass {
		size_t chunkSize = 0;
		int liveChunks = 0;
		std::vector<std::unique_ptr<Pool>> pools; // index is the pool index stored in block headers
		Pool* partialPools = nullptr;
	};

	SizeClass* classFor(size_t size);
	void markPartial(SizeClass& sizeClass, Pool& pool);
	static void* takeChunk(Pool& pool);

	std::vector<std::unique_ptr<SizeClass>> m_classes;
	std::vector<SizeClass*> m_classBySize; // size in bytes -> class
	size_t m_maxPooledSize = 0;
};

} // namespace dice::hfe::memory
