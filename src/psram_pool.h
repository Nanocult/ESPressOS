/**
 * @file psram_pool.h
 * @brief ESP-AppOS PSRAM Block Pool Allocator
 * 
 * Fixed-size block allocator for Octal PSRAM designed for dynamic app loading.
 * Eliminates fragmentation by using uniform blocks with bitmap tracking.
 */

#ifndef ESPAPPOS_PSRAM_POOL_H
#define ESPAPPOS_PSRAM_POOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Block size: 4KB matches SDMMC sector size and Xtensa page alignment */
#define PSRAM_POOL_BLOCK_SIZE   4096U

/** Maximum pool size: 7MB (leaves 1MB for kernel/display/audio in N16R8) */
#define PSRAM_POOL_MAX_BLOCKS   1792U  /* 7MB / 4KB */

/**
 * Initialize the PSRAM block pool.
 * Must be called once during kernel startup, before any app loads.
 * 
 * @param pool_size_bytes  Total PSRAM to dedicate to app pool (must be multiple of BLOCK_SIZE)
 * @return 0 on success, -1 on failure
 */
int psram_pool_init(size_t pool_size_bytes);

/**
 * Allocate contiguous blocks from the pool.
 * 
 * @param size       Requested bytes (rounded up to next block boundary)
 * @param alignment  Required alignment (must be power-of-2, max BLOCK_SIZE)
 * @return Pointer to allocated memory, or NULL if insufficient contiguous blocks
 */
void* psram_pool_alloc_aligned(size_t size, size_t alignment);

/** Convenience wrapper: allocates with BLOCK_SIZE alignment */
static inline void* psram_pool_alloc(size_t size) {
    return psram_pool_alloc_aligned(size, PSRAM_POOL_BLOCK_SIZE);
}

/**
 * Free previously allocated block(s).
 * Safe to call with NULL. Double-free is detected and logged (no crash).
 * 
 * @param ptr  Pointer returned by psram_pool_alloc/alloc_aligned
 */
void psram_pool_free(void* ptr);

/**
 * Reallocate. Preserves data up to min(old_size, new_size).
 * May return same pointer if expansion fits in-place.
 * 
 * @param ptr       Existing allocation (NULL = new alloc)
 * @param new_size  New requested size
 * @return New pointer, or NULL on failure (original ptr remains valid)
 */
void* psram_pool_realloc(void* ptr, size_t new_size);

/** Get total free bytes (contiguous or not) */
size_t psram_pool_free_size(void);

/** Get largest contiguous free block in bytes */
size_t psram_pool_max_free_block(void);

/** Dump pool state to log (for debugging fragmentation) */
void psram_pool_dump_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* ESPAPPOS_PSRAM_POOL_H */
