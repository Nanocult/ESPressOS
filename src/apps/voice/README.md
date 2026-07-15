# Voice Assistant App

It requires concurrent operations (UI rendering, microphone capture, network streaming, and audio playback) which pushes the boundaries of our single-threaded app sandbox. 

## Voice implementation

We must introduce **Kernel API Extensions** for WebSockets and App-level Threading, and establish a strategy for linking `libopus` into a `-nostdlib` environment.

### 1.  Kernel API Extensions

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

### 2. Opus Integration Strategy (No Stdlib) (`opus_wrapper.h`)

Standard `libopus` relies on `malloc`/`free`. Since our apps use `-nostdlib`, we must compile `libopus` as a static library (`libopus_pic.a`) with `-fPIC` and inject our kernel memory allocator.

*Build Note:* Your CMake toolchain must compile `libopus` sources with `-DUSE_ALLOCA=1` (or custom alloc hooks) and link it into the `.espapp` binary.

### 3. The Voice Assistant App (`app_voice.c`)

This app uses a background thread to handle the real-time streaming loop, keeping the LVGL UI thread completely unblocked.

### 4. Server Protocol Contract

Your backend WebSocket endpoint must handle this exact binary/text protocol:

| Direction | Type | Payload | Meaning |
| :--- | :--- | :--- | :--- |
| **Client → Server** | Binary | Opus Frames (20ms) | Continuous audio stream while button held |
| **Client → Server** | Text | `{"event": "end"}` | *(Optional)* Sent when button released if not using connection close |
| **Server → Client** | Text | `{"status": "processing"}` | Acknowledgment that audio is being transcribed |
| **Server → Client** | Binary | Opus Frames (20ms) | TTS Audio response stream |
| **Server → Client** | Text | `{"status": "done"}` | Signals end of TTS stream, app returns to IDLE |

---

## Voice Wake Up 

The **Voice Wake Word ("Hey ESP")** is a hands-free activation of the Voice Assistant using the ESP-SR library, integrating it permanently into the kernel's Audio Engine so the device can wake from low-power states. To achieve hands-free activation, we must fundamentally change how the kernel handles audio. Previously, apps owned the microphone when they needed it. Now, the kernel must **permanently tap the microphone stream** to feed a neural network (WakeNet) in the background, and automatically launch the Voice Assistant app when the wake word is detected.

### 1. Audio Pipeline Redesign: The Mic Multiplexer

We must update `svc_audio.c` so the I2S DMA task acts as a central router. It reads from the physical microphone and broadcasts the PCM data to two destinations: **WakeNet** (when idle) and the **App Ringbuffer** (when an app is actively recording).

**Update `svc_audio.c`:**
```c
/* Add to svc_audio.c global state */
static volatile bool s_app_mic_active = false;
extern void svc_wake_feed_audio(const int16_t* pcm, size_t len); /* Forward declaration */

/* Replace the existing I2S RX capture task with this Multiplexer */
static void mic_capture_task(void* arg) {
    int16_t mic_buf[512]; /* ~32ms of 16kHz audio */
    size_t bytes_read = 0;

    while (1) {
        /* Read directly from I2S DMA */
        i2s_channel_read(s_rx_handle, mic_buf, sizeof(mic_buf), &bytes_read, portMAX_DELAY);
        
        if (bytes_read > 0) {
            /* 1. Feed WakeNet (ONLY if no app is currently holding the mic) */
            if (!s_app_mic_active) {
                svc_wake_feed_audio(mic_buf, bytes_read);
            }
            
            /* 2. Feed App Ringbuffer (If an app called mic_open) */
            if (s_app_mic_active) {
                xRingbufferSend(s_capture_rb, mic_buf, bytes_read, 0);
            }
        }
    }
}

/* Update svc_audio_mic_open/close to toggle the multiplexer state */
k_err_t svc_audio_mic_open(const k_audio_format_t* fmt, k_audio_handle_t* h) { 
    s_app_mic_active = true; 
    *h = (k_audio_handle_t)0xMIC_IN; 
    return K_OK; 
}

k_err_t svc_audio_mic_close(k_audio_handle_t h) { 
    s_app_mic_active = false; 
    return K_OK; 
}
```


### 2. The Wake Word Service (`svc_wake.c`)

This service runs permanently on Core 1. It uses Espressif's **ESP-SR** library (specifically the Audio Front End + WakeNet) to detect the phrase "Hi ESP" or "Hey ESP". 

*Prerequisite: Add `espressif/esp-sr` to your kernel's `idf_component.yml`.*

### 3. Kernel API Extension: The "Auto-Start" Flag

The Voice Assistant app needs to know *why* it was launched. If the user tapped the icon, it should show the "Hold to Talk" UI. If the kernel launched it via Wake Word, it should **bypass the UI and start recording immediately**.

**Update `kernel_api.h` (Bump ABI to v1.5):**
```c
/* Add to k_sys_api_t (append only): */
typedef struct {
    // ... existing fields ...
    
    /** Returns true if the app was launched by the Wake Word service.
     *  Automatically clears the flag after reading. */
    bool (*was_woken_by_voice)(void);
} k_sys_api_t;
```

**Wire it in `kernel_main.c`:**
```c
.sys = {
    // ...
    .was_woken_by_voice = svc_wake_was_triggered,
}
```

### 4. Updating the Voice Assistant App (`app_voice.c`)

We modify the `app_main` entry point to check the wake flag and auto-start the streaming thread.

```c
/* Add to app_main() in app_voice.c, right after UI creation: */

    /* Check if we were woken up by "Hey ESP" */
    if (api->sys.was_woken_by_voice()) {
        api->sys.log(2, "Voice", "Auto-starting via Wake Word!");
        
        /* Bypass UI button press - simulate "Pressed" state */
        g_is_recording = true;
        api->display.obj_set_prop(g_lbl_status, "text", "Listening...");
        
        /* Auto-spawn the streaming thread */
        api->sys.thread_create(stream_thread_fn, NULL, "voice_stream", 8192, &g_stream_thread);
    }
```

### 5. System Chime Implementation

To provide audio feedback that the device is listening, add a simple PCM chime player to `svc_audio.c`. 

```c
/* Embed a short, 16kHz 16-bit mono WAV/PCM array in the kernel binary */
extern const uint8_t chime_pcm_start[] asm("_binary_chime_wav_start");
extern const uint8_t chime_pcm_end[]   asm("_binary_chime_wav_end");

void svc_audio_play_system_chime(void) {
    size_t len = chime_pcm_end - chime_pcm_start;
    /* Send directly to the playback ringbuffer. 
       Because it's a short burst, it will mix or play immediately 
       depending on your DMA task implementation. */
    xRingbufferSend(s_playback_rb, chime_pcm_start, len, pdMS_TO_TICKS(100));
}
```
*(Note: In your CMake, use `target_add_binary_data` to embed `chime.wav` as a raw byte array).*

### 6. Power & Thermal Management (Crucial for Consumer Devices)

Running a neural network continuously on the ESP32-S3 will cause the chip to run warm and drain battery if portable. 

**Optimization Strategy:**
1.  **Light Sleep Integration:** Configure the ESP32-S3 to enter **Light Sleep** when the display is off and no audio is playing. 
2.  **Hardware I2S Wakeup:** The ESP32-S3 can keep the I2S peripheral and a small DMA buffer active during Light Sleep. When the DMA buffer fills, it triggers an RTC interrupt to wake the CPU cores, feed WakeNet, and go back to sleep.
3.  **VAD (Voice Activity Detection):** The ESP-SR AFE includes a hardware-accelerated VAD. Configure the AFE to *only* run the heavy WakeNet model when the VAD detects human speech frequencies, dropping CPU usage by ~60% during quiet periods.

```c
/* In svc_wake.c AFE Config: */
afe_config.vad_init = true;
afe_config.vad_min_speech_ms = 200; 
/* Only process WakeNet if VAD state is SPEECH */
```

---

## Validation & Stress Testing

| Test Scenario | Expected Behavior | Architecture Validated |
| :--- | :--- | :--- |
| **UI Responsiveness** | Dragging/animating UI while recording | Thread isolation (LVGL task vs Stream task) |
| **Network Dropout** | Kill WiFi while recording | `ws.recv` times out gracefully, thread exits, no kernel panic |
| **Force Quit (SD Eject)** | Pull SD card while speaking | Kernel kills `voice_stream` task, frees PSRAM, returns to Launcher |
| **Memory Leak** | Record/Play cycle 50 times | `psram_pool_dump_stats()` shows 0 growth |
| **Audio Glitches** | Listen for pops during TTS playback | Ringbuffer sizing and non-blocking `audio.write` logic |

