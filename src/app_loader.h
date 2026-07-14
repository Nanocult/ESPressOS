/**
 * @file app_loader.h
 * @brief ESP-AppOS Application Loader & Relocator
 */

#ifndef ESPAPPOS_APP_LOADER_H
#define ESPAPPOS_APP_LOADER_H

#include "kernel_api.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum size of a single loaded app (code + rodata + data + bss) */
#define APP_MAX_LOAD_SIZE   (2UL * 1024 * 1024)  /* 2 MB */

/** Loader result codes */
typedef enum {
    LOAD_OK = 0,
    LOAD_ERR_FILE_OPEN,
    LOAD_ERR_HEADER_READ,
    LOAD_ERR_BAD_MAGIC,
    LOAD_ERR_BAD_CRC,
    LOAD_ERR_ABI_MISMATCH,
    LOAD_ERR_TOO_LARGE,
    LOAD_ERR_PSALLOC,
    LOAD_ERR_BODY_READ,
    LOAD_ERR_SIGNATURE,
    LOAD_ERR_RELOC,
    LOAD_ERR_NO_ENTRY,
} load_result_t;

/** Loaded app context - opaque to callers */
typedef struct app_context app_context_t;

/**
 * Load, verify, relocate, and prepare an app for execution.
 * 
 * @param path      Absolute path to .espapp file on SD card
 * @param out_ctx   Receives allocated app context on success
 * @return LOAD_OK on success, error code otherwise
 * 
 * On success, the app is fully relocated in PSRAM and ready to run.
 * Call app_loader_run() to transfer execution.
 * On failure, all resources are cleaned up automatically.
 */
load_result_t app_loader_load(const char* path, app_context_t** out_ctx);

/**
 * Execute a loaded app. Calls app_main() with kernel API pointer.
 * This function returns when the app calls request_exit() or crashes.
 * 
 * @param ctx   App context from successful app_loader_load()
 */
void app_loader_run(app_context_t* ctx);

/**
 * Unload an app: call app_deinit(), free PSRAM, invalidate context.
 * Safe to call even if app crashed.
 * 
 * @param ctx   App context. Set to NULL after this call.
 */
void app_loader_unload(app_context_t* ctx);

/**
 * Get human-readable error string for load_result_t.
 */
const char* app_loader_error_str(load_result_t err);

#ifdef __cplusplus
}
#endif

#endif /* ESPAPPOS_APP_LOADER_H */
