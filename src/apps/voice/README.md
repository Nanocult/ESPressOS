# Voice Assistant App

This is the most complex app yet. It requires concurrent operations (UI rendering, microphone capture, network streaming, and audio playback) which pushes the boundaries of our single-threaded app sandbox. 

To achieve this, we must introduce **Kernel API Extensions** for WebSockets and App-level Threading, and establish a strategy for linking `libopus` into a `-nostdlib` environment.

## 1.  Kernel API Extensions

Update `kernel_api.h` and bump `KERNEL_ABI_VERSION` to `((1 << 8) | 3)` (v1.3).

```c
/* --- ADD TO kernel_api.h --- */

/** WebSocket Message Types */
typedef enum { K_WS_MSG_TEXT = 0, K_WS_MSG_BIN = 1 } k_ws_msg_type_t;
typedef void* k_ws_handle_t;

/** WebSocket API Sub-table */
typedef struct {
    k_err_t (*open)(const char* url, k_ws_handle_t* handle);
    k_err_t (*send)(k_ws_handle_t handle, const void* data, size_t len, k_ws_msg_type_t type);
    k_err_t (*recv)(k_ws_handle_t handle, void* buf, size_t buf_size, size_t* out_len, k_ws_msg_type_t* out_type, uint32_t timeout_ms);
    k_err_t (*close)(k_ws_handle_t handle);
} k_ws_api_t;

/** Threading API (Add to k_sys_api_t) */
typedef void* k_thread_t;
typedef void (*k_thread_fn_t)(void* arg);

typedef struct {
    // ... existing fields ...
    
    /** Spawn a background thread. Tracked by kernel for cleanup on app exit. */
    k_err_t (*thread_create)(k_thread_fn_t fn, void* arg, const char* name, uint32_t stack_size, k_thread_t* out_thread);
    
    /** Wait for thread to finish. */
    k_err_t (*thread_join)(k_thread_t thread);
} k_sys_api_t;

/** Add to master KernelAPI struct (append only!): */
typedef struct {
    // ... existing fields ...
    k_ws_api_t ws;
} KernelAPI;
```

> ⚠️ **Critical Kernel-Side Requirement:** When an app calls `api->sys.thread_create`, the kernel **must** add the resulting `TaskHandle_t` to a registry tied to the active `app_context_t`. If the app crashes or is force-quit (e.g., SD eject), `app_loader_unload()` must iterate this registry and call `vTaskDelete()` on all lingering app threads to prevent PSRAM leaks and kernel panics.

## 2. Opus Integration Strategy (No Stdlib) (`opus_wrapper.h`)

Standard `libopus` relies on `malloc`/`free`. Since our apps use `-nostdlib`, we must compile `libopus` as a static library (`libopus_pic.a`) with `-fPIC` and inject our kernel memory allocator.

*Build Note:* Your CMake toolchain must compile `libopus` sources with `-DUSE_ALLOCA=1` (or custom alloc hooks) and link it into the `.espapp` binary.

## 3. The Voice Assistant App (`app_voice.c`)

This app uses a background thread to handle the real-time streaming loop, keeping the LVGL UI thread completely unblocked.

## 4. Server Protocol Contract

Your backend WebSocket endpoint must handle this exact binary/text protocol:

| Direction | Type | Payload | Meaning |
| :--- | :--- | :--- | :--- |
| **Client → Server** | Binary | Opus Frames (20ms) | Continuous audio stream while button held |
| **Client → Server** | Text | `{"event": "end"}` | *(Optional)* Sent when button released if not using connection close |
| **Server → Client** | Text | `{"status": "processing"}` | Acknowledgment that audio is being transcribed |
| **Server → Client** | Binary | Opus Frames (20ms) | TTS Audio response stream |
| **Server → Client** | Text | `{"status": "done"}` | Signals end of TTS stream, app returns to IDLE |

## 5. Validation & Stress Testing

| Test Scenario | Expected Behavior | Architecture Validated |
| :--- | :--- | :--- |
| **UI Responsiveness** | Dragging/animating UI while recording | Thread isolation (LVGL task vs Stream task) |
| **Network Dropout** | Kill WiFi while recording | `ws.recv` times out gracefully, thread exits, no kernel panic |
| **Force Quit (SD Eject)** | Pull SD card while speaking | Kernel kills `voice_stream` task, frees PSRAM, returns to Launcher |
| **Memory Leak** | Record/Play cycle 50 times | `psram_pool_dump_stats()` shows 0 growth |
| **Audio Glitches** | Listen for pops during TTS playback | Ringbuffer sizing and non-blocking `audio.write` logic |

