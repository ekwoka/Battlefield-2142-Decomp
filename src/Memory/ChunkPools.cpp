#include "ChunkPools.h"

#include <iterator>

namespace dice::hfe::memory {

namespace {

// Size classes in bytes, including bf_malloc's 4-byte block header.
constexpr size_t kSizeClasses[] = {4, 8, 12, 16, 20, 24, 28, 32, 48, 58, 64, 68, 72, 76, 80, 84, 88};

size_t roundUpTo4(size_t size)
{
	return (size + 3) & ~size_t(3);
}

} // namespace

ChunkPools::ChunkPools()
{
	m_maxPooledSize = kSizeClasses[std::size(kSizeClasses) - 1];
	m_classBySize.resize(m_maxPooledSize + 1, nullptr);

	size_t smallestUnassigned = 0;
	for (size_t classSize : kSizeClasses) {
		auto sizeClass = std::make_unique<SizeClass>();
		sizeClass->chunkSize = roundUpTo4(classSize);
		for (; smallestUnassigned <= classSize; ++smallestUnassigned) {
			m_classBySize[smallestUnassigned] = sizeClass.get();
		}
		m_classes.push_back(std::move(sizeClass));
	}
}

ChunkPools::~ChunkPools() = default;

ChunkPools::SizeClass* ChunkPools::classFor(size_t size)
{
	if (size == 0 || size > m_maxPooledSize) {
		return nullptr;
	}
	return m_classBySize[size];
}

void ChunkPools::markPartial(SizeClass& sizeClass, Pool& pool)
{
	if (!pool.onPartialList) {
		pool.onPartialList = true;
		pool.nextPartial = sizeClass.partialPools;
		sizeClass.partialPools = &pool;
	}
}

void* ChunkPools::takeChunk(Pool& pool)
{
	if (!pool.slab) {
		// Thread every chunk of the new slab onto the free list.
		pool.slab = std::make_unique<std::byte[]>(pool.chunkSize * pool.capacity);
		for (int i = 0; i < pool.capacity; ++i) {
			std::byte* chunk = pool.slab.get() + i * pool.chunkSize;
			std::byte* next = i + 1 < pool.capacity ? chunk + pool.chunkSize : nullptr;
			*reinterpret_cast<void**>(chunk) = next;
		}
		pool.freeList = pool.slab.get();
	}
	void* chunk = pool.freeList;
	pool.freeList = *static_cast<void**>(chunk);
	++pool.used;
	return chunk;
}

ChunkPools::Allocation ChunkPools::allocate(size_t size)
{
	SizeClass* sizeClass = classFor(size);
	if (!sizeClass) {
		return {};
	}

	// Prefer a pool known to have room; otherwise walk the chain from the
	// first pool, growing it when every pool is full.
	Pool* pool = sizeClass->partialPools;
	int poolIndex = 0;
	if (pool) {
		if (pool->used + 1 >= pool->capacity) {
			pool->onPartialList = false;
			sizeClass->partialPools = pool->nextPartial;
		}
		poolIndex = pool->index;
	}
	else {
		for (;; ++poolIndex) {
			if (poolIndex == static_cast<int>(sizeClass->pools.size())) {
				if (poolIndex >= MaxPoolsPerClass) {
					return {};
				}
				auto fresh = std::make_unique<Pool>();
				fresh->index = poolIndex;
				fresh->chunkSize = sizeClass->chunkSize;
				fresh->capacity = static_cast<int>(SlabSize / sizeClass->chunkSize);
				sizeClass->pools.push_back(std::move(fresh));
				markPartial(*sizeClass, *sizeClass->pools.back());
			}
			pool = sizeClass->pools[poolIndex].get();
			if (pool->used < pool->capacity) {
				break;
			}
		}
	}

	++sizeClass->liveChunks;
	return {takeChunk(*pool), poolIndex};
}

bool ChunkPools::free(void* chunk, size_t size, int poolIndex)
{
	SizeClass* sizeClass = classFor(size);
	if (!sizeClass || poolIndex < 0 || poolIndex >= static_cast<int>(sizeClass->pools.size())) {
		return false;
	}
	Pool& pool = *sizeClass->pools[poolIndex];
	*static_cast<void**>(chunk) = pool.freeList;
	pool.freeList = chunk;
	--pool.used;
	--sizeClass->liveChunks;
	markPartial(*sizeClass, pool);
	return true;
}

} // namespace dice::hfe::memory
