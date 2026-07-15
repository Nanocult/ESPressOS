# Memory Leak Detector 

Automatically tracking PSRAM allocations per app and reporting leaks on exit? pool allocator tracks allocations per app, reports leaks on deinit. In a dynamically loading OS, a memory leak in a single app shouldn't just degrade performance—it shouldn't be allowed to crash the system or prevent other apps from loading. If a downloaded 3rd-party app forgets to call `free()`, the kernel must detect this upon exit, report it forensically, and **force-reclaim** the memory to protect the PSRAM pool.

To achieve this without adding overhead to every single `malloc` call, we will extend our `psram_pool` block allocator with an **Ownership Tagging System**.

### 1. The Golden Rule of Memory Boundaries
Before writing code, we must enforce a strict architectural boundary:
*   **App Pool (Tagged):** Memory allocated via `api->mem.malloc`. Belongs to the app. Tracked and force-freed on exit.
*   **Kernel Pool (Untagged):** Memory allocated internally by kernel services (e.g., LVGL nodes created via `api->display.obj_create`, Audio ringbuffers). Belongs to the kernel. Survives app transitions. Apps *never* receive raw pointers to kernel memory; they only receive opaque handles.

### 2. Enhancing the PSRAM Pool (`psram_pool.c`)

We add an `owner_id` to the allocation header. The kernel sets a global "Active Owner" before jumping into the app's thread. Every `malloc` stamps the current owner into the header.

#### Update `psram_pool.h`
```c
/* Add to psram_pool.h */

#define PSRAM_OWNER_KERNEL  0
#define PSRAM_OWNER_INVALID 0xFFFFFFFF

/** Set the active owner ID. All subsequent allocations get this tag. */
void psram_pool_set_active_owner(uint32_t owner_id);

/** Get total bytes currently allocated by a specific owner. */
size_t psram_pool_get_leaked_bytes(uint32_t owner_id);

/** Force-free all allocations belonging to a specific owner. 
 *  Returns the number of blocks forcefully reclaimed. */
uint32_t psram_pool_force_free_owner(uint32_t owner_id);
```

#### Update `psram_pool.c`
```c
/* Add to internal alloc_header_t */
typedef struct alloc_header {
    uint32_t start_block;
    uint32_t num_blocks;
    uint32_t magic;
    uint32_t owner_id;      /* <--- NEW: Tags the allocation to an app */
    struct alloc_header* next;
} alloc_header_t;

/* Add global state */
static volatile uint32_t g_active_owner_id = PSRAM_OWNER_KERNEL;

void psram_pool_set_active_owner(uint32_t owner_id) {
    g_active_owner_id = owner_id;
}

/* Modify psram_pool_alloc_aligned to stamp the owner */
void* psram_pool_alloc_aligned(size_t size, size_t alignment) {
    // ... [existing allocation logic] ...
    
    /* Stamp the header */
    alloc_header_t* hdr = (alloc_header_t*)(g_pool.pool_base + start * PSRAM_POOL_BLOCK_SIZE);
    hdr->start_block = start;
    hdr->num_blocks = nblocks;
    hdr->magic = HEADER_MAGIC_ALLOCATED;
    hdr->owner_id = g_active_owner_id; /* <--- NEW */
    hdr->next = g_pool.header_list;
    g_pool.header_list = hdr;
    
    // ... [return user pointer] ...
}

/* Implement Leak Detection & Force-Free */
uint32_t psram_pool_force_free_owner(uint32_t owner_id) {
    if (owner_id == PSRAM_OWNER_KERNEL) return 0; /* Never force-free kernel! */
    
    xSemaphoreTake(g_pool.mutex, portMAX_DELAY);
    
    uint32_t reclaimed_blocks = 0;
    alloc_header_t** pp = &g_pool.header_list;
    
    while (*pp) {
        alloc_header_t* hdr = *pp;
        if (hdr->owner_id == owner_id && hdr->magic == HEADER_MAGIC_ALLOCATED) {
            /* Unlink from list */
            *pp = hdr->next; 
            
            /* Clear bitmap */
            for (uint32_t i = hdr->start_block; i < hdr->start_block + hdr->num_blocks; i++) {
                bitmap_clear(i);
            }
            g_pool.used_blocks -= hdr->num_blocks;
            reclaimed_blocks += hdr->num_blocks;
            
            hdr->magic = HEADER_MAGIC_FREED;
            /* Note: We don't advance *pp because we just replaced it with hdr->next */
        } else {
            pp = &(*pp)->next;
        }
    }
    
    xSemaphoreGive(g_pool.mutex);
    return reclaimed_blocks;
}

size_t psram_pool_get_leaked_bytes(uint32_t owner_id) {
    xSemaphoreTake(g_pool.mutex, portMAX_DELAY);
    size_t leaked = 0;
    alloc_header_t* hdr = g_pool.header_list;
    while (hdr) {
        if (hdr->owner_id == owner_id && hdr->magic == HEADER_MAGIC_ALLOCATED) {
            leaked += (hdr->num_blocks * PSRAM_POOL_BLOCK_SIZE) - HEADER_SIZE;
        }
        hdr = hdr->next;
    }
    xSemaphoreGive(g_pool.mutex);
    return leaked;
}
```

### 3. App Sandbox Integration (`app_sandbox.c`)

The sandbox assigns a unique ID to every loaded app. It sets the active owner before execution, and performs the leak audit *after* all app threads have been joined.

#### Update `app_context_t` (in `app_loader.h`)
```c
struct app_context {
    void*       psram_base;
    size_t      total_ram;
    uint32_t    entry_offset;
    char        name[32];
    bool        is_loaded;
    uint32_t    app_id;     /* <--- NEW: Unique ownership tag */
};
```

#### Update `app_loader_load` (in `app_loader.c`)
```c
/* Add a static counter to generate unique IDs */
static uint32_t s_next_app_id = 1; 

// Inside app_loader_load(), right before returning LOAD_OK:
ctx->app_id = s_next_app_id++;
```

#### Update `app_sandbox_run` (in `app_sandbox.c`)
This is the critical teardown sequence. We must ensure background threads are dead before auditing memory, otherwise a thread might allocate memory *during* the audit.

```c
#include "psram_pool.h"

/* Forward declaration of thread registry join (from Phase 2.4) */
extern void kernel_join_all_app_threads(void); 

load_result_t app_sandbox_run(app_context_t* ctx) {
    // ... [existing setup and task creation] ...
    
    /* 1. Claim ownership of the PSRAM pool for this app */
    psram_pool_set_active_owner(ctx->app_id);
    
    /* 2. Execute app */
    entry(api);
    
    /* 3. App returned. Wait for any background threads to finish.
     *    This is crucial: threads might still be calling malloc! */
    kernel_join_all_app_threads();
    
    /* 4. Relinquish ownership back to Kernel */
    psram_pool_set_active_owner(PSRAM_OWNER_KERNEL);
    
    /* 5. AUDIT FOR LEAKS */
    size_t leaked_bytes = psram_pool_get_leaked_bytes(ctx->app_id);
    if (leaked_bytes > 0) {
        ESP_LOGE("SANDBOX", "🚨 MEMORY LEAK DETECTED in app '%s'!", ctx->name);
        ESP_LOGE("SANDBOX", "   Leaked: %u bytes. Force-reclaiming...", leaked_bytes);
        
        /* 6. FORCE FREE to protect the system */
        uint32_t reclaimed_blocks = psram_pool_force_free_owner(ctx->app_id);
        ESP_LOGW("SANDBOX", "   Reclaimed %u blocks (%u bytes).", 
                 reclaimed_blocks, reclaimed_blocks * PSRAM_POOL_BLOCK_SIZE);
                 
        /* Optional: Write leak report to SD card for telemetry */
        // write_leak_report(ctx->name, leaked_bytes);
    }

    // ... [existing teardown and return] ...
}
```

### 4. Handling the "Kernel Service" Edge Case

What if an app calls `api->display.obj_create("label", NULL)`? 
LVGL needs memory to create that label. If LVGL calls `malloc`, it will accidentally be tagged with the **App's Owner ID**, causing the kernel's display manager to lose its UI nodes when the app exits and the sandbox force-frees the memory!

**The Solution: Kernel API Wrappers**
The kernel's implementation of the Display/Audio APIs must temporarily switch the owner ID to `PSRAM_OWNER_KERNEL` before calling internal allocation functions, then switch it back.

**Update `svc_display.c`:**
```c
k_lvgl_obj_t svc_display_obj_create(const char* type, k_lvgl_obj_t parent) {
    /* 1. Pause App Ownership */
    uint32_t prev_owner = g_active_owner_id; /* Read via a getter function */
    psram_pool_set_active_owner(PSRAM_OWNER_KERNEL);
    
    /* 2. Kernel allocates LVGL node (LVGL uses custom alloc hooks routed to psram_pool) */
    lv_obj_t* obj = NULL;
    if (strcmp(type, "label") == 0) obj = lv_label_create(parent);
    // ...
    
    /* 3. Restore App Ownership */
    psram_pool_set_active_owner(prev_owner);
    
    return (k_lvgl_obj_t)obj;
}
```
*Note: The same pattern applies to `svc_audio_open_output` or any kernel service that allocates state on behalf of the app.*

### 5. Validation & Telemetry

To prove this works, intentionally introduce a leak into the `app_clock.c` we built in Phase 2:

```c
/* Inside app_clock.c app_main() */
void* leak = g_api->mem.malloc(8192); /* Intentional leak */
g_api->sys.log(2, "Clock", "Allocated 8KB and lost the pointer!");
```

**Expected Console Output on App Exit:**
```text
I (14502) LIFECYCLE: Unloading app 'clock'
I (14505) app_sandbox: ◀ Sandbox: 'clock' exited normally
E (14510) SANDBOX: 🚨 MEMORY LEAK DETECTED in app 'clock'!
E (14518) SANDBOX:    Leaked: 8192 bytes. Force-reclaiming...
W (14525) SANDBOX:    Reclaimed 3 blocks (12288 bytes).
I (14532) psram_pool: Free 3 blocks @ block 12 (0x3f80c000)
I (14540) LIFECYCLE: PSRAM Pool Stats: Used 0 blocks, Fragmentation 0.0%
```

### 6. Why This Architecture is Bulletproof

| Threat | Mitigation |
| :--- | :--- |
| **App forgets to `free()`** | Detected on exit. Force-freed. Pool remains at 100% capacity. |
| **App crashes (abort/panic)** | Sandbox catches panic, joins threads, runs leak audit, force-frees. |
| **App spawns thread that leaks** | Sandbox `kernel_join_all_app_threads()` blocks until thread dies, *then* audits. |
| **Kernel service leaks** | Tagged as `PSRAM_OWNER_KERNEL`. Never force-freed. (Kernel leaks require firmware OTA to fix, but are rare since kernel code is tightly controlled). |
| **App passes pointer to Kernel** | Prevented by architecture. Kernel APIs copy data or use opaque handles. Kernel never stores raw app-pool pointers. |

