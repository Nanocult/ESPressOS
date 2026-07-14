# Proposed Technical Specifications

## Tech Stack

**PSRAM Module**: ESP32-S3-WROOM-1-N16R8 (16MB Flash + 8MB Octal PSRAM)
**Framework**: ESP-IDF v5.3+ (v5.x has significantly better PSRAM management)
**Filesystem**: LittleFS on SD (more wear-leveling resilient than FAT for frequent app writes)
**Display**: LVGL v9 with custom PSRAM buffer driver
**Audio**: ESP-ADF or esp-idf-audio with ringbuffer in PSRAM
**Voice**: ESP-SR (wake word + command recognition) + Opus encoding for server streaming
**Networking**: MQTT for real-time server events, HTTPS for app downloads

> Octal PSRAM provides ~40MB/s bandwidth vs Quad’s ~10MB/s. This is non-negotiable for streaming audio from SD while loading apps. 8MB is the sweet spot: enough for app code + LVGL buffers + audio ringbuffers without the cost/power penalty of 16MB PSRAM variants. Avoid N8R2 (2MB PSRAM is insufficient for dynamic apps).

## Display & Architecture Configurations

| Feature | Config A: "Value" (Single MCU) | Config B: "Premium" (Dual MCU) |
| :--- | :--- | :--- |
| **MCU** | 1× ESP32-S3-N16R8 | 1× S3-N16R8 (App Core) + 1× S3-N8R2 (IO Core) |
| **Display** | 2.8" ST7789 SPI 240×320 RGB565 | 3.5" ILI9488 8-bit Parallel 320×480 RGB565 |
| **Audio** | INMP441 PDM Mic + MAX98357A DAC | ES8311 I2S Codec (ADC+DAC) + INMP441 Wake Word |
| **PSRAM Usage** | ~3MB display buf + ~4MB apps | App Core: Full 8MB for apps; IO Core: Dedicated display/audio DMA |
| **Load Time** | ~300-500ms (SPI display yields bus) | <150ms (Parallel display on separate MCU, no contention) |
| **UI Quality** | Good, but tearing possible during loads | Excellent, 60FPS guaranteed even during app swap |
| **BOM Cost** | ~$12-15 | ~$22-28 |
| **Complexity** | Low | High (IPC via UART/SPI between MCUs) |

> **Strong Recommendation: Start with Config A.**
> Dual-MCU adds enormous complexity (IPC protocol, sync issues, dual debugging, dual OTA). The S3-N16R8 with Octal PSRAM can handle Config A *smoothly* if we use proper DMA buffering. Only move to Config B if user testing proves UI jank is unacceptable. The plan below targets Config A with a clear migration path to B.

## Audio Codec Choice
**INMP441 (PDM Mic) + MAX98357A (I2S DAC)**
*   **Why not ES8311?** For voice assistant + playback, separate PDM mic + I2S amp is superior: PDM mics have better SNR for far-field voice, MAX98357A has built-in class-D amp (no external speaker driver), and they share the same I2S bus with TDM mode. Saves GPIO vs ES8311 which needs separate ADC/DAC clocks.
*   **Wake Word:** Use second INMP441 or share via TDM slot. ESP-SR supports multi-mic PDM natively.

---

