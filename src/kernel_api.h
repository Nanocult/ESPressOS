/**
 * @file kernel_api.h
 * @brief ESP-AppOS Kernel Application Binary Interface v1.0
 * 
 * THIS FILE DEFINES THE STABLE ABI BETWEEN KERNEL AND APPS.
 * - Apps access ALL kernel services exclusively through this struct.
 * - Apps MUST NOT include ESP-IDF headers or call system functions directly.
 * - Adding new functions: APPEND ONLY to the end of KernelAPI struct.
 * - Removing/reordering functions: FORBIDDEN (bump ABI version instead).
 * - All function pointers use __attribute__((cdecl)) for Xtensa compatibility.
 */

/* TODO:
Action Required: Bump KERNEL_ABI_VERSION_MINOR to 1 in kernel_api.h and build_espapp.py. 
Update kernel_main.c to wire svc_clock_get_datetime and a simple k_itoa implementation into g_kernel_api.
*/

#ifndef ESPAPPOS_KERNEL_API_H
#define ESPAPPOS_KERNEL_API_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* ABI VERSIONING                                                             */
/* ========================================================================== */

#define KERNEL_ABI_VERSION_MAJOR 1
#define KERNEL_ABI_VERSION_MINOR 0

/* Increment MAJOR on breaking changes, MINOR on backward-compatible additions */
#define KERNEL_ABI_VERSION ((KERNEL_ABI_VERSION_MAJOR << 8) | KERNEL_ABI_VERSION_MINOR)

/* ========================================================================== */
/* COMMON TYPES & ERROR CODES                                                 */
/* ========================================================================== */

typedef int32_t k_err_t;

#define K_OK             0
#define K_ERR_NOMEM     -1
#define K_ERR_INVALID   -2
#define K_ERR_TIMEOUT   -3
#define K_ERR_NOT_FOUND -4
#define K_ERR_PERM      -5
#define K_ERR_IO        -6
#define K_ERR_BUSY      -7
#define K_ERR_EOF       -8
#define K_ERR_SIG       -9   /* Signature verification failed */
#define K_ERR_ABI       -10  /* ABI version mismatch */

/** Opaque handle types - apps never dereference these */
typedef void* k_file_t;
typedef void* k_audio_handle_t;
typedef void* k_timer_t;
typedef void* k_lvgl_obj_t;  /* LVGL object handle */

/** Audio format descriptor */
typedef struct {
    uint32_t sample_rate;   /* e.g., 16000, 44100 */
    uint8_t  channels;      /* 1 = mono, 2 = stereo */
    uint8_t  bits_per_sample; /* 16 or 32 */
    uint8_t  codec;         /* 0=PCM, 1=Opus, 2=MP3 */
    uint8_t  reserved;
} k_audio_format_t;

/** File open modes */
#define K_FILE_READ      0x01
#define K_FILE_WRITE     0x02
#define K_FILE_APPEND    0x04
#define K_FILE_CREATE    0x08

/* ========================================================================== */
/* MEMORY MANAGEMENT                                                          */
/* Apps MUST use these instead of malloc/free.                                */
/* Allocates from PSRAM app heap pool only.                                   */
/* ========================================================================== */

typedef struct {
    /** Allocate from app PSRAM pool. Returns NULL on failure. */
    void* (*malloc)(size_t size);
    
    /** Allocate aligned memory from app PSRAM pool. */
    void* (*malloc_aligned)(size_t size, size_t alignment);
    
    /** Free memory back to app PSRAM pool. NULL-safe. */
    void (*free)(void* ptr);
    
    /** Get remaining free bytes in app PSRAM pool. */
    size_t (*free_heap_size)(void);
    
    /** Reallocate. Semantics match standard realloc. */
    void* (*realloc)(void* ptr, size_t new_size);
} k_mem_api_t;

/* ========================================================================== */
/* DISPLAY / LVGL INTERFACE                                                   */
/* All display operations go through LVGL. Direct framebuffer access forbidden.*/
/* ========================================================================== */

typedef struct {
    /** Initialize LVGL screen for this app. Call once in app_main. */
    k_err_t (*init_screen)(uint16_t width, uint16_t height);
    
    /** Create LVGL object. Returns opaque handle. */
    k_lvgl_obj_t (*obj_create)(const char* type, k_lvgl_obj_t parent);
    
    /** Set object property by name. Value is string-encoded. */
    k_err_t (*obj_set_prop)(k_lvgl_obj_t obj, const char* prop, const char* value);
    
    /** Register callback for object event. */
    k_err_t (*obj_on_event)(k_lvgl_obj_t obj, uint32_t event_mask, 
                            void (*callback)(k_lvgl_obj_t obj, uint32_t event, void* user_data),
                            void* user_data);
    
    /** Force screen refresh. Call after batch UI updates. */
    void (*flush)(void);
    
    /** Destroy all objects created by current app. Called automatically on unload. */
    void (*cleanup)(void);
} k_display_api_t;

/* ========================================================================== */
/* AUDIO ENGINE                                                               */
/* Persistent service. Audio continues during app transitions.                */
/* ========================================================================== */

typedef struct {
    /** Open audio output stream. Returns handle. */
    k_err_t (*open_output)(const k_audio_format_t* fmt, k_audio_handle_t* handle);
    
    /** Write PCM/encoded data to playback buffer. Blocks if full. */
    k_err_t (*write)(k_audio_handle_t handle, const void* data, size_t len, 
                     uint32_t timeout_ms);
    
    /** Close audio handle. Stops playback for this handle. */
    k_err_t (*close)(k_audio_handle_t handle);
    
    /** Start microphone capture. Returns handle. */
    k_err_t (*mic_open)(const k_audio_format_t* fmt, k_audio_handle_t* handle);
    
    /** Read captured audio data. Blocks if empty. */
    k_err_t (*mic_read)(k_audio_handle_t handle, void* buf, size_t len, 
                        size_t* bytes_read, uint32_t timeout_ms);
    
    /** Stop and close microphone. */
    k_err_t (*mic_close)(k_audio_handle_t handle);
    
    /** Set master volume (0-100). Persists across apps. */
    k_err_t (*set_volume)(uint8_t volume);
} k_audio_api_t;

/* ========================================================================== */
/* FILESYSTEM (SD CARD)                                                       */
/* Sandboxed: apps can only access /data/<app_name>/ and shared /media/       */
/* ========================================================================== */

typedef struct {
    /** Open file. Path relative to app sandbox root. */
    k_err_t (*open)(const char* path, uint32_t flags, k_file_t* file);
    
    /** Read from file. Returns bytes read or negative error. */
    k_err_t (*read)(k_file_t file, void* buf, size_t len, size_t* bytes_read);
    
    /** Write to file. Returns bytes written or negative error. */
    k_err_t (*write)(k_file_t file, const void* buf, size_t len, size_t* bytes_written);
    
    /** Seek within file. */
    k_err_t (*seek)(k_file_t file, int32_t offset, uint8_t whence);
    
    /** Close file handle. */
    k_err_t (*close)(k_file_t file);
    
    /** Check if path exists. */
    bool (*exists)(const char* path);
    
    /** Create directory (recursive). */
    k_err_t (*mkdir)(const char* path);
} k_fs_api_t;

/* ========================================================================== */
/* NETWORKING                                                                 */
/* Simplified HTTP/MQTT interface. Raw sockets not exposed.                   */
/* ========================================================================== */

typedef struct {
    /** HTTP GET. Response buffered in caller-provided buffer. */
    k_err_t (*http_get)(const char* url, void* resp_buf, size_t buf_size, 
                        size_t* resp_len, uint32_t timeout_ms);
    
    /** HTTP POST with body. */
    k_err_t (*http_post)(const char* url, const void* body, size_t body_len,
                         void* resp_buf, size_t buf_size, size_t* resp_len,
                         uint32_t timeout_ms);
    
    /** Stream HTTP response via callback (for large/audio responses). */
    k_err_t (*http_stream)(const char* url, 
                           k_err_t (*on_chunk)(void* user_data, const void* data, size_t len),
                           void* user_data, uint32_t timeout_ms);
    
    /** Check WiFi connectivity status. */
    bool (*wifi_connected)(void);
} k_net_api_t;

/* ========================================================================== */
/* SYSTEM SERVICES                                                            */
/* ========================================================================== */

typedef struct {
    /** Millisecond tick counter (wraps at ~49 days). */
    uint32_t (*tick_ms)(void);
    
    /** Delay current task. Yields to scheduler. */
    void (*delay_ms)(uint32_t ms);
    
    /** Log message. Level: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG. */
    void (*log)(uint8_t level, const char* tag, const char* fmt, ...);
    
    /** Request graceful app exit. Kernel will call deinit then unload. */
    void (*request_exit)(void);
    
    /** Get current app name (as declared in manifest). */
    const char* (*get_app_name)(void);
    
    /** Create periodic timer. Returns handle. */
    k_err_t (*timer_create)(uint32_t period_ms, bool repeat,
                            void (*callback)(void* user_data), void* user_data,
                            k_timer_t* timer);
    
    /** Cancel and destroy timer. */
    k_err_t (*timer_delete)(k_timer_t timer);

    /** Safe integer to string conversion (base 10) */
    void (*itoa)(int val, char* buf, int min_width);
} k_sys_api_t;

/** Self-contained datetime struct (replaces struct tm) */
typedef struct {
    uint16_t year;    /* e.g., 2026 */
    uint8_t  month;   /* 1-12 */
    uint8_t  day;     /* 1-31 */
    uint8_t  hour;    /* 0-23 */
    uint8_t  min;     /* 0-59 */
    uint8_t  sec;     /* 0-59 */
    uint8_t  wday;    /* 0-6 (0=Sunday) */
    bool     is_synced;
} k_datetime_t;

/** Clock API Sub-table */
typedef struct {
    k_err_t (*get_datetime)(k_datetime_t* out_dt);
    bool (*is_synced)(void);
} k_clock_api_t;

/* ========================================================================== */
/* MASTER KERNEL API STRUCTURE                                                */
/* This is the ONLY symbol exported to apps.                                  */
/* Apps receive pointer to this in app_main().                                */
/* ORDER MATTERS: Append new sub-APIs or functions ONLY at the end.           */
/* ========================================================================== */

typedef struct {
    /** ABI version. Apps MUST check this before using any other field. */
    uint32_t abi_version;
    
    /** Sub-API tables */
    k_mem_api_t     mem;
    k_display_api_t display;
    k_audio_api_t   audio;
    k_fs_api_t      fs;
    k_net_api_t     net;
    k_sys_api_t     sys;

    k_clock_api_t clock;
    
    /* FUTURE EXTENSIONS: Add new sub-API pointers here. 
     * Apps compiled against older ABI will have these as NULL.
     * Always check for NULL before calling extended APIs. */
} KernelAPI;

/* ========================================================================== */
/* APP ENTRY POINT PROTOTYPE                                                  */
/* Every .espapp MUST define this function.                                   */
/* ========================================================================== */

/**
 * Application entry point. Called by kernel after successful load + relocation.
 * 
 * @param api Pointer to KernelAPI table. Valid for entire app lifetime.
 *            Store this pointer globally; do NOT copy the struct.
 * 
 * App should:
 * 1. Verify api->abi_version >= expected version
 * 2. Initialize UI via api->display.init_screen()
 * 3. Return when initialization complete (app continues running via callbacks/timers)
 * 
 * To exit: call api->sys.request_exit(). Do NOT return from app_main to exit.
 */
void app_main(const KernelAPI* api);

/**
 * Optional deinitialization hook. Called by kernel before unload.
 * If defined, kernel calls this with 500ms timeout.
 * Release all resources, stop timers, flush buffers.
 * Linker attribute ensures it's discoverable without symbol table.
 */
void app_deinit(void) __attribute__((weak));

#ifdef __cplusplus
}
#endif

#endif /* ESPAPPOS_KERNEL_API_H */
