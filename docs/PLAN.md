# Step-by-Step Development Plan

## Phase 0: Hardware Validation

| Task | Deliverable | Success Criteria |
| :--- | :--- | :--- |
| 0.1 Procure ESP32-S3-N16R8 devkit + INMP441 + MAX98357A + ST7789 240×320 + SDMMC breakout | Working hardware bench | All peripherals respond to basic test sketches |
| 0.2 Benchmark Octal PSRAM bandwidth with concurrent I2S DMA + SPI DMA | Bandwidth report | Sustained >25MB/s combined throughput |
| 0.3 Validate SDMMC 4-bit raw read speed | Speed benchmark | >8MB/s sequential read at 40MHz |
| 0.4 Profile ESP-SR wake word RAM/CPU footprint on N16R8 | Resource report | Wake word uses <300KB SRAM, <15% CPU |

## Phase 1: Kernel Foundation (Week 3-6)

| Task | Deliverable | Notes |
| :--- | :--- | :--- |
| 1.1 Implement PSRAM block pool allocator | `psram_pool.c/h` | Fixed 4KB blocks, O(1) alloc/free, zero fragmentation |
| 1.2 Build `.espapp` toolchain integration | CMake custom command + linker script | Auto-generates relocation table, signs binary |
| 1.3 Implement app loader + relocator | `app_loader.c` | Loads header → validates sig → copies sections → patches relocs → returns entry fn ptr |
| 1.4 Define Kernel API struct + dispatcher | `kernel_api.h` | Versioned function pointer table, capability-checked |
| 1.5 Persistent services: Clock+NTP, Audio Engine, Display Mgr | 3 FreeRTOS tasks | Pinned to Core 0, use internal SRAM only |
| 1.6 App lifecycle state machine | `lifecycle.c` | INIT→RUNNING→DEINIT→UNLOADED with timeout watchdog |

## Phase 2: Core Apps MVP (Week 7-10)

| Task | App | Key Integration Points |
| :--- | :--- | :--- |
| 2.1 | Clock/Calendar | NTP sync via kernel API, LVGL widget, saves timezone to NVS |
| 2.2 | App Launcher | Reads `/apps/manifest.json` from SD, renders grid, triggers loader |
| 2.3 | Voice Recorder | Captures PDM → Opus encode → writes to SD `/recordings/`, waveform display |
| 2.4 | AI Voice Command | Wake word → record → stream Opus via HTTPS POST → receive audio response → enqueue to audio engine |
| 2.5 | Tasks/Notes Editor | CRUD on SD `/data/tasks.json`, LVGL text input, keyboard support |
| 2.6 | Audio Player | Reads MP3/Opus from SD → decode → audio engine queue, playlist management |

## Phase 3: Robustness & Polish (Week 11-14)

| Task | Deliverable |
| :--- | :--- |
| 3.1 Crash recovery system | NVS-stored last-good-app, auto-fallback on fault |
| 3.2 App signature verification | ED25519 in bootloader + runtime pre-load check |
| 3.3 OTA app update channel | Download to `/ota/pending.espapp` → verify → atomic rename |
| 3.4 Memory leak detector | Pool allocator tracks allocations per app, reports leaks on deinit |
| 3.5 Power management | Deep sleep between voice activations, display dimming |
| 3.6 Stress testing | 1000 app load/unload cycles, SD hot-swap, WiFi dropout recovery |

## Phase 4: Premium Migration Path (Optional, Week 15+)

If Config A UI quality is insufficient:
- Port Display Mgr + Audio Engine to secondary S3-N8R2
- Replace kernel API calls with UART/SPI IPC messages
- App Core becomes headless computation node
- **Zero changes to app code** (Kernel API abstraction preserved)

---

## ⚠️ Critical Next Steps Before Coding

1.  **Order hardware immediately** – Phase 0 validation must happen before any OS code is written. PSRAM bandwidth assumptions need empirical confirmation.
2.  **Finalize server API contract** – Voice command endpoint spec (auth, streaming protocol, response format) blocks Task 2.4.
3.  **Define `.espapp` binary spec document** – This is the ABI contract between kernel and all future apps. Get it right now; changing it later breaks all downloaded apps.
4.  **Set up CI/CD for app builds** – Each app should be a standalone CMake project that produces signed `.espapp` artifacts. Manual builds will not scale.

