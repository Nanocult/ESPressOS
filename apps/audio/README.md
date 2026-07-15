# Audio Player (MP3 decoder)

ESPressOS supports robust multimedia playback with MP3/Opus Decoding (ntegrated `libhelix-mp3`). The app will read MP3 frames, decode them into a local PCM buffer, and feed the exact same `audio.write()` ringbuffer API.

## MP3 decoding (libhelix)

Integrating MP3 decoding transforms the Audio Player from a technical validation tool into a real consumer media app. We will use Helix MP3, a highly optimized, royalty-free decoder widely used in embedded systems. It requires ~30KB of RAM for its state machine, which fits perfectly in our PSRAM pool.
To maintain our 60FPS UI guarantee, we will move all audio decoding (both WAV and MP3) into a dedicated background thread using the k_sys_api_t threading extensions introduced in Phase 2.4.

1. The Helix Wrapper API (helix_wrapper.h)
To keep the app code clean and isolate it from Helix's complex internal macros, we define a minimal wrapper. The actual Helix source code will be compiled and linked by your CMake toolchain.

2. The Universal Audio Player App (app_audio.c)
This updated app scans for both .wav and .mp3, parses headers/formats dynamically, and uses a background thread for uninterrupted playback.

3. Build System Integration (CMake)
You must compile the Helix source files alongside your app code. Add the Helix source directory to your app's CMakeLists.txt:

> ⚠️ **Critical Helix Compilation Flag:** Helix uses floating-point math in some profiles. Ensure you compile with `-DUSE_FLOAT_MATH=0` or `-DFIXED_POINT=1` to force integer-only math, which is vastly faster on the ESP32-S3 and avoids pulling in heavy software FPU libraries.

---

### Validation Matrix

| Test Scenario | Expected Behavior | Architecture Validated |
| :--- | :--- | :--- |
| **MP3 Discovery** | UI lists `.mp3` files with `[MP3]` prefix. | String parsing and directory iteration. |
| **Format Negotiation** | 48kHz Mono MP3 plays at correct pitch/speed. | Dynamic `k_audio_format_t` configuration via `helix_info_t`. |
| **Thread Isolation** | Tapping UI buttons while MP3 decodes causes zero UI lag. | Background thread execution + blocking `audio.write`. |
| **Memory Footprint** | App binary size ~150KB. PSRAM usage ~60KB during playback. | Helix static linking and PSRAM pool allocation. |
| **Track Switching** | Switching from 44k Stereo WAV to 22k Mono MP3 seamlessly reconfigures I2S. | Audio handle teardown and recreation in thread. |
| **Force Quit** | Long-press BACK while MP3 plays. | Thread cancellation via `g_is_playing` flag, clean `app_deinit`. |











