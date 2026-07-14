#ifndef APP_SANDBOX_H
#define APP_SANDBOX_H

#include "app_loader.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum time (ms) an app is allowed to block without yielding */
#define APP_WDT_TIMEOUT_MS      5000

/** Stack size for app tasks - must accommodate LVGL + user code */
#define APP_TASK_STACK_SIZE     (8 * 1024)

/**
 * Run an app inside a sandboxed task with crash protection.
 * Blocks until app exits normally OR crashes.
 * 
 * @param ctx       Loaded app context
 * @return LOAD_OK on normal exit, LOAD_ERR_RELOC on crash/fault
 */
load_result_t app_sandbox_run(app_context_t* ctx);

/** Get path to most recent crash report (empty string if none) */
const char* app_sandbox_last_crash_report(void);

#ifdef __cplusplus
}
#endif
#endif
