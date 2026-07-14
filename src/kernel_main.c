#include "kernel_api.h"
#include "svc_clock.h"
#include "svc_audio.h"
#include "svc_display.h"
#include "psram_pool.h"
#include "app_loader.h"

/* Forward declarations of all service implementations */
extern void* svc_mem_malloc(size_t);
extern void  svc_mem_free(void*);
/* ... etc for all mem/fs/net/sys functions ... */

/* THE SINGLE IMMUTABLE KERNEL API TABLE */
static const KernelAPI g_kernel_api = {
    .abi_version = KERNEL_ABI_VERSION,
    
    .mem = {
        .malloc         = psram_pool_alloc,       /* Routes to our pool */
        .malloc_aligned = psram_pool_alloc_aligned,
        .free           = psram_pool_free,
        .realloc        = psram_pool_realloc,
        .free_heap_size = psram_pool_free_size,
    },
    .display = {
        .init_screen = svc_display_init_screen,
        .obj_create  = svc_display_obj_create,
        .obj_set_prop= NULL, /* TODO: Implement property setter */
        .obj_on_event= NULL, /* TODO: Implement event registration */
        .flush       = svc_display_flush,
        .cleanup     = svc_display_cleanup,
    },
    .audio = {
        .open_output = svc_audio_open_output,
        .write       = svc_audio_write,
        .close       = svc_audio_close,
        .mic_open    = svc_audio_mic_open,
        .mic_read    = svc_audio_mic_read,
        .mic_close   = svc_audio_mic_close,
        .set_volume  = svc_audio_set_volume,
    },
    .fs  = { /* TODO: Wire SD card sandboxed FS */ },
    .net = { /* TODO: Wire HTTP/MQTT wrappers */ },
    .sys = {
        .tick_ms      = svc_clock_tick_ms,
        .delay_ms     = vTaskDelay, /* Direct FreeRTOS pass-through */
        .log          = esp_log_write, /* Or custom wrapper */
        .request_exit = NULL, /* Set by lifecycle manager */
        .get_app_name = NULL, /* Set by lifecycle manager */
        .timer_create = NULL, /* TODO */
        .timer_delete = NULL, /* TODO */
    },
};

const KernelAPI* kernel_get_api(void) { return &g_kernel_api; }

/* ========================================================================== */
/* KERNEL ENTRY POINT                                                         */
/* ========================================================================== */
void app_main(void) {
    ESP_LOGI("KERNEL", "ESP-AppOS starting...");
    
    /* 1. Initialize PSRAM pool FIRST */
    psram_pool_init(7UL * 1024 * 1024); /* 7MB for apps */
    
    /* 2. Initialize persistent services */
    svc_clock_init();
    svc_audio_init();
    svc_display_init();
    
    /* 3. Load initial clock app */
    app_context_t* ctx = NULL;
    load_result_t res = app_loader_load("/sdcard/apps/clock.espapp", &ctx);
    if (res == LOAD_OK) {
        app_loader_run(ctx);      /* Blocks until app exits */
        app_loader_unload(ctx);   /* Clean up */
    } else {
        ESP_LOGE("KERNEL", "Failed to load clock app: %s", app_loader_error_str(res));
    }
    
    /* 4. Enter app launcher loop (Phase 2) */
    ESP_LOGI("KERNEL", "Entering launcher...");
    // launcher_main(); 
}
