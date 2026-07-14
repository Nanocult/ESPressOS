#include "app_sandbox.h"
#include "kernel_api.h"
#include "svc_storage.h"
#include "esp_log.h"
#include "esp_debug_helpers.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char* TAG = "app_sandbox";

/* Shared state between sandbox task and caller */
typedef struct {
    app_context_t*  ctx;
    load_result_t   result;
    SemaphoreHandle_t done_sem;
    TaskHandle_t    app_task_handle;
    volatile bool   crashed;
    char            crash_msg[256];
} sandbox_state_t;

/* ========================================================================== */
/* CRASH REPORT WRITER                                                        */
/* ========================================================================== */
static void write_crash_report(const char* app_name, const char* reason, 
                                uint32_t pc, uint32_t exc_cause) {
    char path[128];
    time_t now; time(&now);
    struct tm tm; localtime_r(&now, &tm);
    
    snprintf(path, sizeof(path), "/sdcard/crash_reports/%04d%02d%02d_%02d%02d%02d_%s.crash",
             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, app_name);
    
    FILE* f = fopen(path, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to write crash report: %s", path);
        return;
    }
    
    fprintf(f, "App: %s\n", app_name);
    fprintf(f, "Reason: %s\n", reason);
    fprintf(f, "PC: 0x%08X\n", pc);
    fprintf(f, "ExcCause: %u\n", exc_cause);
    fprintf(f, "Timestamp: %ld\n", now);
    fprintf(f, "\n--- Backtrace ---\n");
    
    /* Capture backtrace from current task */
    esp_backtrace_print(100);
    
    fclose(f);
    ESP_LOGW(TAG, "Crash report written: %s", path);
}

/* ========================================================================== */
/* APP TASK ENTRY POINT                                                       */
/* ========================================================================== */
static void app_task_entry(void* arg) {
    sandbox_state_t* state = (sandbox_state_t*)arg;
    const KernelAPI* api = kernel_get_api();
    
    /* Register this task with TWDT */
    esp_task_wdt_add(NULL);
    
    typedef void (*app_entry_fn)(const KernelAPI*);
    app_entry_fn entry = (app_entry_fn)(
        (uint8_t*)state->ctx->psram_base + state->ctx->entry_offset);
    
    ESP_LOGI(TAG, "▶ Sandbox: running '%s'", state->ctx->name);
    
    /* Execute app - if it faults, FreeRTOS panic handler catches it */
    entry(api);
    
    /* Normal exit path */
    esp_task_wdt_delete(NULL);
    state->result = LOAD_OK;
    xSemaphoreGive(state->done_sem);
    vTaskDelete(NULL);
}

/* ========================================================================== */
/* PANIC HANDLER HOOK                                                         */
/* Called by ESP-IDF panic handler before reboot. We intercept to recover.    */
/* ========================================================================== */
static sandbox_state_t* s_active_sandbox = NULL;

void app_sandbox_panic_hook(uint32_t pc, uint32_t exc_cause) {
    if (!s_active_sandbox || !s_active_sandbox->ctx) return;
    
    s_active_sandbox->crashed = true;
    snprintf(s_active_sandbox->crash_msg, sizeof(s_active_sandbox->crash_msg),
             "Exception cause %u at PC 0x%08X", exc_cause, pc);
    
    write_crash_report(s_active_sandbox->ctx->name, 
                       s_active_sandbox->crash_msg, pc, exc_cause);
}

/* ========================================================================== */
/* PUBLIC API                                                                 */
/* ========================================================================== */
load_result_t app_sandbox_run(app_context_t* ctx) {
    if (!ctx) return LOAD_ERR_NO_ENTRY;
    
    sandbox_state_t state = {
        .ctx = ctx,
        .result = LOAD_ERR_RELOC,
        .done_sem = xSemaphoreCreateBinary(),
        .crashed = false,
    };
    
    s_active_sandbox = &state;
    
    BaseType_t ret = xTaskCreatePinnedToCore(
        app_task_entry, "app_sandbox", APP_TASK_STACK_SIZE,
        &state, tskIDLE_PRIORITY + 5, &state.app_task_handle, 0);
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create app task");
        s_active_sandbox = NULL;
        vSemaphoreDelete(state.done_sem);
        return LOAD_ERR_PSALLOC;
    }
    
    /* Wait for completion with WDT timeout */
    TickType_t timeout = pdMS_TO_TICKS(APP_WDT_TIMEOUT_MS);
    while (1) {
        if (xSemaphoreTake(state.done_sem, pdMS_TO_TICKS(100)) == pdTRUE) break;
        
        /* Feed WDT - if app task hasn't fed it, TWDT triggers panic hook */
        esp_task_wdt_reset();
        
        /* Check if task has been deleted unexpectedly */
        if (eTaskGetState(state.app_task_handle) == eDeleted && !state.crashed) {
            ESP_LOGE(TAG, "App task vanished without signaling");
            state.result = LOAD_ERR_RELOC;
            break;
        }
    }
    
    s_active_sandbox = NULL;
    vSemaphoreDelete(state.done_sem);
    
    if (state.crashed) {
        ESP_LOGE(TAG, "✖ App '%s' CRASHED: %s", ctx->name, state.crash_msg);
        return LOAD_ERR_RELOC;
    }
    
    ESP_LOGI(TAG, "◀ Sandbox: '%s' exited normally", ctx->name);
    return state.result;
}

static char s_last_crash_path[128] = {0};
const char* app_sandbox_last_crash_report(void) { return s_last_crash_path; }
