# OS Architecture

Given the RAM constraints, we structured a Hybrid Kernel + Dynamic Blob Loader approach rather than true ELF dynamic linking (which is fragile on Xtensa/LX7).

```
┌──────────────────────────────────────────────────────┐
│              KERNEL (Internal SRAM + 1MB PSRAM)      │
│  ┌─────────────┐ ┌──────────────┐ ┌───────────────┐  │
│  │ Clock/NTP   │ │ Audio Engine │ │ Display Mgr   │  │
│  │ (FreeRTOS)  │ │ (RingBuf DMA)│ │ (LVGL+DoubleB)│  │
│  └─────────────┘ └──────────────┘ └───────────────┘  │
│  ┌─────────────────────────────────────────────────┐  │
│  │ App Lifecycle Manager                           │  │
│  │ • .espapp loader/relocator                      │  │
│  │ • PSRAM Pool Allocator (block-based)            │  │
│  │ • Kernel API Dispatcher (fn ptr table)          │  │
│  │ • Crash Recovery + Signature Verify             │  │
│  └─────────────────────────────────────────────────┘  │
├──────────────────────────────────────────────────────┤
│           APP REGION (7MB Octal PSRAM)               │
│  ┌────────────────────────────────────────────────┐   │
│  │ Active App (.espapp loaded + relocated)        │   │
│  │ Max single app size: 2MB (configurable)        │   │
│  └────────────────────────────────────────────────┘   │
│  ┌────────────────────────────────────────────────┐   │
│  │ App Heap Pool (5MB, block allocator)           │   │
│  │ Apps use ONLY this region via kernel API       │   │
│  └────────────────────────────────────────────────┘   │
├──────────────────────────────────────────────────────┤
│         SDMMC 4-BIT (FAT32/LittleFS Hybrid)          │
│  /system/kernel.bin  /apps/*.espapp  /data/*         │
│  /cache/voice_tmp.opus  /ota/pending.espapp         │
└──────────────────────────────────────────────────────┘
```

1. **Apps as Position-Independent Code (PIC) Blobs**: Compile each app with `-fPIC -mlongcalls`. The loader copies the blob to a fixed PSRAM base address and patches a jump table. This avoids the complexity of a full ELF dynamic linker while allowing true unload/reload.
2. **Strict Memory Ownership**: Apps may ONLY allocate from the designated PSRAM pool. Internal SRAM is reserved exclusively for kernel tasks, WiFi/BT stacks, and ISR handlers. Any app attempting `heap_caps_malloc(MALLOC_CAP_INTERNAL)` is terminated.
3. **Kernel Services as C Function Table**: Apps don't link against kernel code. Instead, the loader passes a `const AppKernelAPI*` struct containing function pointers (`api->display_draw`, `api->audio_play`, `api->fs_read`). This decouples app ABI from kernel updates.
4. **Graceful Unload Protocol**: Before unloading, the kernel calls `app_deinit()`. The app must release all PSRAM pool allocations, stop timers, and close file handles within 500ms. Failure triggers forced cleanup + crash logging.
5. **Audio/Clock as Persistent Services**: These run as FreeRTOS tasks in protected memory. Apps interact via message queues, not direct hardware access. This ensures music doesn't stop when opening the task manager.
6. **Brief pause acceptable**: simplifies unload, stop app task → drain audio ringbuf → free PSRAM pool → load next. No need for overlapping execution.
7. **SDMMC 4-bit**: Enables <200ms app loads. Use `sdmmc_host_set_card_clk(40MHz)` for max throughput.
8. **PDM + I2S DAC**: Single I2S peripheral in TDM mode. Audio engine owns it permanently; apps send PCM via queue.
9. **Flat binary format** → Reduces app size by 30-50% vs ELF, critical for SD storage and load time.

---

## Kernel API

The Kernel API header (kernel_api.h) defines the function pointer table and all service interfaces apps will call through this ABI

(TODO: Extend this paragraph)


---

## App Loader Public Interface & Relocator

This module is the heart of ESP-AppOS. It safely loads .espapp binaries from SDMMC into Octal PSRAM, verifies integrity, applies Xtensa LX7 relocations in a single pass, and transfers execution to the app.

### Required Dependencies You Must Provide

| Symbol | Source | Purpose |
|:--------|:--------|:---------|
| `psram_pool_alloc_aligned()` | `psram_pool.c` (Phase 1 Task 1.1) | Contiguous PSRAM allocation |
| `psram_pool_free()` | `psram_pool.c` | Release PSRAM block |
| `kernel_get_api()` | `kernel_main.c` | Returns `const KernelAPI*` |
| `kernel_verify_signature()` | `crypto.c` | ED25519 verify against embedded pubkey |
| `esp_rom_crc32_le()` | ESP-IDF ROM | Hardware-accelerated CRC32 |
| `espapp_format.h` | Generated from spec | Header/reloc struct definitions |

### Known Limitation: `app_deinit` Discovery

The current loader does **not** automatically find `app_deinit()` because it's a weak symbol and its offset isn't stored in the header. Two options:

-   **Immediate fix:** Add `deinit_offset` field to `espapp_header_t` and update `build_espapp.py` to extract it from ELF symbols. Bump ABI version.
-   **Workaround for now:** Have apps register their deinit callback via `api->sys.register_deinit()` during `app_main()`. Store the callback pointer in kernel state.

> ⚠️ **Recommendation:** Implement the `deinit_offset` header field **before** writing any apps. Retrofitting later requires rebuilding all binaries.

### Testing Strategy

| Test Case | Expected Behavior |
|-----------|-------------------|
| Valid signed app | Loads, runs, unloads cleanly |
| Corrupted header CRC | Fails at `LOAD_ERR_BAD_CRC`, no PSRAM leak |
| Wrong ABI version | Fails at `LOAD_ERR_ABI_MISMATCH` |
| Oversized app | Fails at `LOAD_ERR_TOO_LARGE` |
| Invalid signature | Fails at `LOAD_ERR_SIGNATURE`, body never executed |
| Truncated file | Fails at `LOAD_ERR_BODY_READ` |
| Double unload | No crash, second call is no-op |
| Run without load | Logs error, no crash |
| 100 load/unload cycles | Zero PSRAM fragmentation, stable heap watermark |

---

## PSRAM Block Pool Allocator

This allocator is the foundation of ESPressOS memory safety. Standard heap_caps_malloc on ESP32-S3 suffers from severe fragmentation when repeatedly loading/unloading apps of varying sizes. This custom block pool allocator guarantees zero external fragmentation, O(1) allocation/free, and complete memory reclamation after every app unload.

### Architecture Diagram

```
┌─────────────────────── Internal SRAM ───────────────────────┐
│  Bitmap (1792 bits = 224 bytes)  │  Mutex  │  Header List   │
│  [████░░░░████░░░░████████░░░...] │         │  hdr→hdr→NULL  │
└─────────────────────────────────────────────────────────────┘
                          ↕ maps to
┌─────────────────────── Octal PSRAM ─────────────────────────┐
│ Block 0    │ Block 1    │ Block 2    │ ... │ Block 1791    │
│ [HDR|DATA] │ [FREE]     │ [HDR|DATA] │     │ [FREE]        │
│ 4KB        │ 4KB        │ 4KB        │     │ 4KB           │
└─────────────────────────────────────────────────────────────┘
 ↑ User pointer = Block base + HEADER_SIZE (64 bytes)
```

### Key Design Decisions Explained

| Decision | Rationale |
|----------|-----------|
| **4KB block size** | Matches SDMMC sector size (enables direct DMA), satisfies Xtensa alignment, balances granularity vs bitmap overhead |
| **Bitmap in SRAM** | 224 bytes for 7MB pool. Scanning bitmap is ~100× faster than walking PSRAM-linked lists |
| **Header in PSRAM, metadata in SRAM** | Header travels with allocation for validation; bitmap/list stay in fast SRAM for O(1) operations |
| **Magic numbers** | Detects double-free, use-after-free, and buffer underflow without ASAN overhead |
| **In-place realloc** | Avoids copy when expanding into adjacent free blocks — critical for audio buffer growth |
| **Alignment-aware search** | Prevents wasted blocks from misaligned allocations fragmenting the pool |
| **Mutex protection** | Apps may allocate from different tasks; loader runs on separate task from UI |

### Testing Checklist

| Test | Method | Pass Criteria |
|------|--------|---------------|
| Basic alloc/free | Alloc 100 random sizes, free all | `used_blocks == 0`, no corrupt headers |
| Contiguous allocation | Alloc 512KB (128 blocks) | Returns non-NULL, single contiguous region |
| Alignment | Alloc with 8KB alignment | Returned address % 8192 == 0 |
| Double-free detection | Free same pointer twice | Second call logs error, no crash, bitmap unchanged |
| Realloc in-place | Alloc 4KB, realloc to 8KB when adjacent free | Same pointer returned |
| Realloc copy | Alloc 4KB surrounded by used blocks, realloc to 8KB | Different pointer, data preserved |
| Exhaustion | Alloc until NULL | `free_size() == 0`, graceful failure |
| Thread safety | 4 tasks alloc/free concurrently for 60s | No corruption, no deadlock |
| Leak detection | Load/unload 50 apps | `dump_stats()` shows 0 used blocks after each |
| Max free block accuracy | Fragment pool, check value | Matches manual bitmap inspection |

---

## Persistent Services Stubs

These three services form the Protected Kernel Layer. They run permanently in Internal SRAM (or reserved PSRAM), own all hardware peripherals, and never get unloaded. Apps interact with them exclusively through the KernelAPI function pointers. The kernel file (kernel_main.c) wires everything together (populating the API Table)

1. System Clock & NTP Service (svc_clock.c)
This service maintains wall-clock time independently of app lifecycle. It syncs via NTP on WiFi connect and falls back to RTC during offline periods.

2. Audio Engine Service (svc_audio.c)
Owns the I2S peripheral permanently. Uses double-buffered ring buffers in PSRAM so apps can stream audio without glitching during transitions. Supports concurrent playback + mic capture via TDM.

3. Display Manager Service (svc_display.c)
Owns LVGL instance and SPI/parallel display bus. Provides sandboxed object creation so apps cannot corrupt each other's UI state. Double-buffered in PSRAM.

4. Storage Manager Service 
The Storage Manager Service runs persistently in the kernel, monitors the SDMMC bus electrically (not just via filesystem), and orchestrates safe recovery or provisioning.

5. Lifecycle Integration: Reacting to Storage Events (lifecycle_manager.c)
Wire the storage manager into your kernel's lifecycle/app launcher so the UI responds appropriately.

---

## The Crash Isolation Wrapper 

### App Crash Isolation Wrapper (`app_sandbox.h/.c`)

This wrapper runs every app in a dedicated FreeRTOS task with stack canaries, watchdog monitoring, and fault capture. When an app crashes, the kernel recovers gracefully and logs forensic data, ensures a faulty app never kills the kernel.

> ⚠️ **Critical Integration Note:** You must register `app_sandbox_panic_hook` with ESP-IDF's panic handler in `kernel_main.c`:
> ```c
> extern void app_sandbox_panic_hook(uint32_t, uint32_t);
> // In app_main():
> esp_register_panic_handler(app_sandbox_panic_hook);
> ```
> Also initialize TWDT early: `esp_task_wdt_init(CONFIG_ESP_TASK_WDT_TIMEOUT_S, true);`

### Input Manager Service (`svc_input.h/.c`)

The Input Manager provides a centralized, gesture-aware input pipeline that apps subscribe to rather than polling hardware directly. Centralizes all physical input handling. Supports multi-consumer subscriptions, system gesture interception (long-press volume, double-tap wake), and debouncing. Apps receive clean events through the Kernel API. 

#### Wiring Into Kernel API Table

Update `kernel_main.c` to expose these services through the ABI:

```c
/* Add to k_sys_api_t in kernel_api.h FIRST (append only!): */
// int (*input_subscribe)(uint32_t mask, void (*cb)(const void*, void*), void* ud);
// void (*input_unsubscribe)(int sub_id);

/* In kernel_main.c g_kernel_api initialization: */
.sys = {
    // ... existing fields ...
    .input_subscribe   = (void*)svc_input_subscribe,
    .input_unsubscribe = (void*)svc_input_unsubscribe,
},
```

> ⚠️ **ABI Version Bump Required:** Adding fields to `k_sys_api_t` requires incrementing `KERNEL_ABI_VERSION_MINOR` in `kernel_api.h`. All existing apps remain compatible since new fields are appended.

#### Usage Example From an App

```c
/* Inside a .espapp application */
static const KernelAPI* g_api;

static void on_input(const void* raw_evt, void* ud) {
    const input_event_t* evt = (const input_event_t*)raw_evt;
    
    switch (evt->type) {
        case INPUT_EVT_BTN_PRESS:
            g_api->sys.log(2, "MyApp", "Button %d pressed", evt->button.btn_id);
            break;
        case INPUT_EVT_TOUCH_DOWN:
            g_api->sys.log(2, "MyApp", "Touch at %d,%d", evt->touch.x, evt->touch.y);
            break;
        default: break;
    }
}

void app_main(const KernelAPI* api) {
    g_api = api;
    
    /* Subscribe to buttons + touch */
    uint32_t mask = INPUT_EVT_BTN_PRESS | INPUT_EVT_BTN_RELEASE | 
                    INPUT_EVT_TOUCH_DOWN | INPUT_EVT_TOUCH_MOVE;
    api->sys.input_subscribe(mask, on_input, NULL);
    
    /* App continues via callbacks - no polling needed */
}

void app_deinit(void) {
    /* Unsubscribe handled automatically by kernel on unload,
       but explicit unsubscribe is good practice */
}
```

#### Validation Checklist 

| Test | Pass Criteria |
| :--- | :--- |
| App calls `abort()` | Sandbox catches fault, writes crash report, kernel continues |
| App infinite loop | TWDT triggers after 5s, crash report generated |
| App stack overflow | Canary detects, panic hook fires, recovery succeeds |
| Rapid button presses | No bounce artifacts, correct press/release pairs |
| Long press BTN_UP | Volume increases, event NOT forwarded to app |
| Long press BACK > 2s | Force-quit gesture consumed, app unloaded |
| 16 subscribers active | All receive matching events, no missed deliveries |
| Eject SD during crash report write | Partial file cleaned up, no FS corruption |
| Normal app exit | Sandbox returns LOAD_OK, no resources leaked |
| Repeated crash/reload cycle | No memory growth over 50 iterations |

---

### Critical Integration Notes

| Concern | Solution |
| :--- | :--- |
| **Service Memory Isolation** | All service buffers allocated with `MALLOC_CAP_INTERNAL` or dedicated PSRAM region *outside* the app pool. Apps physically cannot touch them. |
| **Audio Glitch Prevention** | Ringbuffer decouples app write timing from DMA read. Silence filler prevents DAC pops when no app is playing. |
| **LVGL Object Leaks** | `s_app_objects[]` array tracks every widget. `cleanup()` deletes all before next app loads. No manual memory management for apps. |
| **Thread Safety** | Each service has its own mutex. Audio DMA task runs at highest priority. LVGL runs on dedicated task. Apps call API from their own task. |
| **Hardware Ownership** | Services initialize peripherals once at boot. Apps never touch GPIO/I2S/SPI registers directly. Violation = crash (by design). |
| **NTP Resilience** | Sync status tracked independently. Clock continues ticking via RTC during WiFi outages. Apps query sync status before displaying network time. |

### Critical Design Decisions & Safety Guarantees

| Concern | Solution | Why It Matters |
| :--- | :--- | :--- |
| **No Auto-Format** | `format_if_mount_failed = false` always | Prevents accidental data loss when user inserts card with photos/docs |
| **Atomic Provisioning** | Download to `/cache/*.tmp` → verify signature → rename | Partial downloads don't corrupt app directory; power-loss safe |
| **CD Pin Polling (Not Interrupt)** | 100ms poll in dedicated task | SDMMC CD pins are notoriously noisy; polling with debounce is more reliable than ISR-based detection on ESP32-S3 |
| **Health Checks While Mounted** | Periodic read/write test every 5s | Detects silent corruption/ejection without CD pin change (e.g., loose contact) |
| **Force-Unload on Eject** | Immediate `app_loader_unload()` | Prevents app from reading/writing stale FS handles → avoids FAT corruption |
| **Provision Flag File** | `.provisioned` marker after successful bootstrap | Distinguishes "fresh card" from "card where user deleted apps intentionally" |
| **Separate Monitor Task** | Dedicated FreeRTOS task at low priority | Storage events never block audio DMA, display refresh, or app execution |
| **Cluster Size Alignment** | 16KB allocation unit on format | Matches PSRAM pool block size × 4 → optimal sequential read performance for app loading |

### Server Provisioning Protocol Requirements

Your server must expose these endpoints for provisioning to work:

```
GET /api/v1/bootstrap/manifest.json
→ Returns: {"apps": ["clock.espapp", "launcher.espapp", ...], "version": "1.0.0"}

GET /api/v1/apps/{name}.espapp
→ Returns: Raw binary stream with Content-Length header
→ MUST serve signed .espapp binaries matching your ED25519 key

GET /api/v1/apps/{name}.espapp.sig
→ Optional: Separate signature if not embedded in binary
```

> ⚠️ **Security Note:** The provisioning downloader MUST verify the ED25519 signature of each `.espapp` *before* moving it from cache to the apps directory. A malicious server or MITM attack could otherwise push trojaned apps to every device that provisions from it.

### Testing Matrix

| Scenario | Expected Behavior | Verification |
| :--- | :--- | :--- |
| Boot with no card | Shows "Insert SD Card" overlay | Display renders overlay within 500ms |
| Insert formatted+provisioned card | Mounts → loads launcher/clock | App starts within 1s of insertion |
| Insert blank/unformatted card | Shows "Download apps?" dialog | Dialog appears, no auto-format |
| User confirms provisioning | Formats → downloads → shows progress → loads launcher | All bootstrap apps functional after completion |
| Eject card while app running | App force-unloaded → "Insert card" overlay | No crash, no FS corruption, clean recovery |
| Eject + reinsert same card | Remounts → resumes launcher | State preserved correctly |
| Insert corrupted FS card | Treated as unformatted → offers provision | No silent failures |
| Power loss during provisioning | On reboot: partial files in `/cache`, no broken apps in `/apps` | Atomic rename guarantee verified |
| Loose SD contact (intermittent CD) | Health check detects I/O error → shows error overlay | Recovers when contact restored |

---

### 🏗️ Architectural Improvements

| Issue | Current State | Improved Design |
| :--- | :--- | :--- |
| **LVGL Thread Safety** | Apps call LVGL directly from their task → race conditions with flush callback | All LVGL calls routed through display service mutex + dedicated LVGL task. Apps submit commands via queue. |
| **App Crash Isolation** | App fault crashes entire kernel | Wrap `app_loader_run()` in task with stack canary + watchdog. Fault triggers graceful unload + crash report to SD. |
| **Audio Format Negotiation** | Hardcoded 44.1kHz stereo | Apps declare required format in manifest. Audio engine resamples or rejects mismatch. |
| **Storage Provisioning Security** | Downloads directly to apps dir | Download → SHA256 verify → ED25519 verify → atomic rename. Three-stage pipeline. |
| **Debug Visibility** | Only serial log | Add kernel debug overlay (toggle with long-press button): shows FPS, RAM, active app, storage state. |

---

### 💡 Suggested Additional Features 

TODO: implement these kernel-level capabilities that are nearly impossible to retrofit later:

1.  **App Manifest Parser**: Load `/apps/<name>.manifest.json` before binary. Contains: version, min ABI, required permissions, icon path, display resolution hint. Enables launcher grid rendering without loading app code.
2.  **Crash Reporter Service**: On app fault, capture PC, backtrace, registers, and last 256 log lines. Write to `/sdcard/crash_reports/<timestamp>.crash`. Next boot shows "App X crashed" notification.
3.  **Input Manager Service**: Centralize button/touch/encoder handling. Apps register interest via `api->input.subscribe(mask, callback)`. Kernel routes events, handles long-press system gestures (volume, brightness, force-quit).
4.  **Power Manager Integration**: Dim display after 30s idle. Deep sleep after 5min. Wake on touch/button/mic wake-word. Expose `api->sys.set_idle_timeout()` for apps (e.g., audio player disables sleep).
5.  **Kernel Self-Test Mode**: Boot-time validation of PSRAM pool, audio DMA, display flush, SD read/write. Shows green/red status screen. Essential for manufacturing QA and field diagnostics.
