#include "psram_pool.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <assert.h>

static const char* TAG = "psram_pool";

/* ========================================================================== */
/* INTERNAL DATA STRUCTURES                                                   */
/* ========================================================================== */

/** Allocation header stored BEFORE each user block.
 *  Lives in internal SRAM to avoid PSRAM read overhead on free(). */
typedef struct alloc_header {
    uint32_t start_block;     /* First block index of this allocation */
    uint32_t num_blocks;      /* Number of contiguous blocks */
    uint32_t magic;           /* Integrity check */
    struct alloc_header* next;/* Linked list for iteration/debug */
} alloc_header_t;

#define HEADER_MAGIC_ALLOCATED  0xA110CA7E  /* "ALLOCATE" */
#define HEADER_MAGIC_FREED      0xF2EEB10C  /* "FREEBLOC" */
#define HEADER_SIZE             sizeof(alloc_header_t)

/** Pool state - single static instance */
static struct {
    uint8_t*    pool_base;        /* PSRAM base address */
    uint32_t    total_blocks;     /* Total blocks in pool */
    uint32_t    used_blocks;      /* Currently allocated blocks */
    
    /** Bitmap: 1 = allocated, 0 = free. 
     *  Uses uint32_t words for efficient scanning. */
    uint32_t*   bitmap;
    uint32_t    bitmap_words;
    
    /** Header linked list head (for debug/validation) */
    alloc_header_t* header_list;
    
    /** Thread safety */
    SemaphoreHandle_t mutex;
    
    bool initialized;
} g_pool;

/* ========================================================================== */
/* BITMAP OPERATIONS                                                          */
/* ========================================================================== */

static inline bool bitmap_get(uint32_t idx) {
    return (g_pool.bitmap[idx >> 5] >> (idx & 31)) & 1U;
}

static inline void bitmap_set(uint32_t idx) {
    g_pool.bitmap[idx >> 5] |= (1U << (idx & 31));
}

static inline void bitmap_clear(uint32_t idx) {
    g_pool.bitmap[idx >> 5] &= ~(1U << (idx & 31));
}

/** Find N contiguous free blocks starting from search_start.
 *  Returns start index or UINT32_MAX if not found. */
static uint32_t find_contiguous_free(uint32_t nblocks, uint32_t alignment_blocks) {
    uint32_t run_start = 0;
    uint32_t run_len = 0;
    
    /* Align starting position */
    uint32_t aligned_start = 0;
    if (alignment_blocks > 1) {
        aligned_start = (run_start + alignment_blocks - 1) & ~(alignment_blocks - 1);
    }
    
    for (uint32_t i = aligned_start; i < g_pool.total_blocks; i++) {
        if (!bitmap_get(i)) {
            if (run_len == 0) run_start = i;
            run_len++;
            if (run_len >= nblocks) return run_start;
        } else {
            run_len = 0;
            /* Skip ahead to next aligned position */
            if (alignment_blocks > 1) {
                uint32_t next_aligned = (i + 1 + alignment_blocks - 1) & ~(alignment_blocks - 1);
                if (next_aligned > i + 1) {
                    i = next_aligned - 1; /* Loop increment will add 1 */
                    run_len = 0;
                }
            }
        }
    }
    return UINT32_MAX;
}

/* ========================================================================== */
/* PUBLIC API                                                                 */
/* ========================================================================== */

int psram_pool_init(size_t pool_size_bytes) {
    if (g_pool.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return 0;
    }
    
    /* Validate and align pool size */
    uint32_t blocks = pool_size_bytes / PSRAM_POOL_BLOCK_SIZE;
    if (blocks == 0 || blocks > PSRAM_POOL_MAX_BLOCKS) {
        ESP_LOGE(TAG, "Invalid pool size: %u bytes (%u blocks, max %u)",
                 pool_size_bytes, blocks, PSRAM_POOL_MAX_BLOCKS);
        return -1;
    }
    
    /* Allocate pool memory in Octal PSRAM */
    g_pool.pool_base = (uint8_t*)heap_caps_malloc(
        blocks * PSRAM_POOL_BLOCK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_pool.pool_base) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes PSRAM", blocks * PSRAM_POOL_BLOCK_SIZE);
        return -1;
    }
    
    /* Allocate bitmap in INTERNAL SRAM (fast access, small footprint) */
    g_pool.bitmap_words = (blocks + 31) / 32;
    g_pool.bitmap = (uint32_t*)heap_caps_malloc(
        g_pool.bitmap_words * sizeof(uint32_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_32BIT);
    if (!g_pool.bitmap) {
        heap_caps_free(g_pool.pool_base);
        ESP_LOGE(TAG, "Failed to allocate bitmap");
        return -1;
    }
    memset(g_pool.bitmap, 0, g_pool.bitmap_words * sizeof(uint32_t));
    
    /* Create mutex */
    g_pool.mutex = xSemaphoreCreateMutex();
    if (!g_pool.mutex) {
        heap_caps_free(g_pool.pool_base);
        heap_caps_free(g_pool.bitmap);
        ESP_LOGE(TAG, "Failed to create mutex");
        return -1;
    }
    
    g_pool.total_blocks = blocks;
    g_pool.used_blocks = 0;
    g_pool.header_list = NULL;
    g_pool.initialized = true;
    
    ESP_LOGI(TAG, "✓ Pool initialized: %u blocks (%u KB) @ %p",
             blocks, blocks * (PSRAM_POOL_BLOCK_SIZE / 1024), g_pool.pool_base);
    return 0;
}

void* psram_pool_alloc_aligned(size_t size, size_t alignment) {
    if (!g_pool.initialized || size == 0) return NULL;
    
    /* Clamp alignment to block size */
    if (alignment > PSRAM_POOL_BLOCK_SIZE) alignment = PSRAM_POOL_BLOCK_SIZE;
    if (alignment < 1) alignment = 1;
    
    /* Calculate blocks needed (include header) */
    size_t total_size = HEADER_SIZE + size;
    uint32_t nblocks = (total_size + PSRAM_POOL_BLOCK_SIZE - 1) / PSRAM_POOL_BLOCK_SIZE;
    uint32_t align_blocks = alignment / PSRAM_POOL_BLOCK_SIZE;
    if (align_blocks < 1) align_blocks = 1;
    
    xSemaphoreTake(g_pool.mutex, portMAX_DELAY);
    
    uint32_t start = find_contiguous_free(nblocks, align_blocks);
    if (start == UINT32_MAX) {
        xSemaphoreGive(g_pool.mutex);
        ESP_LOGW(TAG, "Alloc failed: %u bytes (%u blocks), max free = %u bytes",
                 size, nblocks, psram_pool_max_free_block());
        return NULL;
    }
    
    /* Mark blocks as allocated */
    for (uint32_t i = start; i < start + nblocks; i++) {
        bitmap_set(i);
    }
    g_pool.used_blocks += nblocks;
    
    /* Place header at start of allocation */
    alloc_header_t* hdr = (alloc_header_t*)(g_pool.pool_base + start * PSRAM_POOL_BLOCK_SIZE);
    hdr->start_block = start;
    hdr->num_blocks = nblocks;
    hdr->magic = HEADER_MAGIC_ALLOCATED;
    hdr->next = g_pool.header_list;
    g_pool.header_list = hdr;
    
    xSemaphoreGive(g_pool.mutex);
    
    /* Return pointer AFTER header */
    void* user_ptr = (uint8_t*)hdr + HEADER_SIZE;
    ESP_LOGD(TAG, "Alloc %u bytes @ block %u (%u blocks) → %p", size, start, nblocks, user_ptr);
    return user_ptr;
}

void psram_pool_free(void* ptr) {
    if (!ptr || !g_pool.initialized) return;
    
    /* Recover header */
    alloc_header_t* hdr = (alloc_header_t*)((uint8_t*)ptr - HEADER_SIZE);
    
    xSemaphoreTake(g_pool.mutex, portMAX_DELAY);
    
    /* Validate header */
    if (hdr->magic != HEADER_MAGIC_ALLOCATED) {
        if (hdr->magic == HEADER_MAGIC_FREED) {
            ESP_LOGE(TAG, "DOUBLE FREE detected @ %p (block %u)", ptr, hdr->start_block);
        } else {
            ESP_LOGE(TAG, "CORRUPT HEADER @ %p (magic 0x%08X)", ptr, hdr->magic);
        }
        xSemaphoreGive(g_pool.mutex);
        return;
    }
    
    /* Clear bitmap */
    for (uint32_t i = hdr->start_block; i < hdr->start_block + hdr->num_blocks; i++) {
        bitmap_clear(i);
    }
    g_pool.used_blocks -= hdr->num_blocks;
    
    /* Remove from linked list */
    alloc_header_t** pp = &g_pool.header_list;
    while (*pp && *pp != hdr) pp = &(*pp)->next;
    if (*pp) *pp = hdr->next;
    
    /* Poison header to detect use-after-free */
    hdr->magic = HEADER_MAGIC_FREED;
    
    xSemaphoreGive(g_pool.mutex);
    
    ESP_LOGD(TAG, "Free %u blocks @ block %u (%p)", hdr->num_blocks, hdr->start_block, ptr);
}

void* psram_pool_realloc(void* ptr, size_t new_size) {
    if (!ptr) return psram_pool_alloc(new_size);
    if (new_size == 0) { psram_pool_free(ptr); return NULL; }
    
    alloc_header_t* hdr = (alloc_header_t*)((uint8_t*)ptr - HEADER_SIZE);
    size_t old_user_size = (hdr->num_blocks * PSRAM_POOL_BLOCK_SIZE) - HEADER_SIZE;
    
    /* If shrinking or same size, return same pointer */
    if (new_size <= old_user_size) return ptr;
    
    /* Try in-place expansion: check if adjacent blocks are free */
    size_t total_needed = HEADER_SIZE + new_size;
    uint32_t new_nblocks = (total_needed + PSRAM_POOL_BLOCK_SIZE - 1) / PSRAM_POOL_BLOCK_SIZE;
    uint32_t extra_blocks = new_nblocks - hdr->num_blocks;
    
    xSemaphoreTake(g_pool.mutex, portMAX_DELAY);
    
    bool can_expand = true;
    uint32_t next_block = hdr->start_block + hdr->num_blocks;
    for (uint32_t i = 0; i < extra_blocks && can_expand; i++) {
        if ((next_block + i) >= g_pool.total_blocks || bitmap_get(next_block + i)) {
            can_expand = false;
        }
    }
    
    if (can_expand) {
        /* Expand in place */
        for (uint32_t i = 0; i < extra_blocks; i++) {
            bitmap_set(next_block + i);
        }
        g_pool.used_blocks += extra_blocks;
        hdr->num_blocks = new_nblocks;
        xSemaphoreGive(g_pool.mutex);
        ESP_LOGD(TAG, "Realloc in-place: %u → %u blocks @ %p", 
                 hdr->num_blocks - extra_blocks, new_nblocks, ptr);
        return ptr;
    }
    
    xSemaphoreGive(g_pool.mutex);
    
    /* Cannot expand: allocate new, copy, free old */
    void* new_ptr = psram_pool_alloc(new_size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, old_user_size);
        psram_pool_free(ptr);
    }
    return new_ptr;
}

size_t psram_pool_free_size(void) {
    if (!g_pool.initialized) return 0;
    return (g_pool.total_blocks - g_pool.used_blocks) * PSRAM_POOL_BLOCK_SIZE;
}

size_t psram_pool_max_free_block(void) {
    if (!g_pool.initialized) return 0;
    
    xSemaphoreTake(g_pool.mutex, portMAX_DELAY);
    
    uint32_t max_run = 0, cur_run = 0;
    for (uint32_t i = 0; i < g_pool.total_blocks; i++) {
        if (!bitmap_get(i)) {
            cur_run++;
            if (cur_run > max_run) max_run = cur_run;
        } else {
            cur_run = 0;
        }
    }
    
    xSemaphoreGive(g_pool.mutex);
    return max_run * PSRAM_POOL_BLOCK_SIZE;
}

void psram_pool_dump_stats(void) {
    if (!g_pool.initialized) {
        ESP_LOGW(TAG, "Pool not initialized");
        return;
    }
    
    xSemaphoreTake(g_pool.mutex, portMAX_DELAY);
    
    uint32_t alloc_count = 0;
    alloc_header_t* h = g_pool.header_list;
    while (h) { alloc_count++; h = h->next; }
    
    ESP_LOGI(TAG, "=== PSRAM Pool Stats ===");
    ESP_LOGI(TAG, "Total: %u blocks (%u KB)", g_pool.total_blocks, 
             g_pool.total_blocks * (PSRAM_POOL_BLOCK_SIZE / 1024));
    ESP_LOGI(TAG, "Used:  %u blocks (%u KB) | Free: %u blocks (%u KB)",
             g_pool.used_blocks, g_pool.used_blocks * (PSRAM_POOL_BLOCK_SIZE / 1024),
             g_pool.total_blocks - g_pool.used_blocks,
             (g_pool.total_blocks - g_pool.used_blocks) * (PSRAM_POOL_BLOCK_SIZE / 1024));
    ESP_LOGI(TAG, "Active allocations: %u", alloc_count);
    ESP_LOGI(TAG, "Max contiguous free: %u bytes", psram_pool_max_free_block());
    ESP_LOGI(TAG, "Fragmentation: %.1f%%", 
             g_pool.total_blocks > 0 ? 
             100.0f * (1.0f - (float)psram_pool_max_free_block() / 
                       ((g_pool.total_blocks - g_pool.used_blocks) * PSRAM_POOL_BLOCK_SIZE + 1)) : 0.0f);
    
    xSemaphoreGive(g_pool.mutex);
}
