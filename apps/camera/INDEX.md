# Camera App

## Core Features

| Feature | Implementation |
|:--------|:---------------|
| **Photo Capture** | OV2640 JPEG mode → Write to `/media/photos/YYYYMMDD_HHMMSS.jpg` |
| **Video Recording** | MJPEG stream (JPEG frames + WAV audio) → Container file |
| **Gallery** | Thumbnail grid, swipe to browse, tap to view full-screen |
| **Social Sharing** | OAuth2 token storage, multipart/form-data upload |
| **Camera Settings** | Resolution, JPEG quality, white balance, exposure, effects |
| **Voice Commands** | "Take picture", "Record video", "Post to [platform]", "Open gallery" |
| **Touch Interface** | Shutter button, mode switch (Photo/Video), settings gear, gallery icon |
| **Background Upload** | Queue system, retry with exponential backoff |

### Kernel Services Required

1. **Camera Service** (`svc_camera.c`)
   - DVP initialization, framebuffer management
   - JPEG encoder configuration
   - MJPEG stream capture
   - Power management (camera sleep when idle)

2. **Media Encoder Service** (`svc_media_encoder.c`)
   - JPEG quality control
   - EXIF metadata injection (timestamp, device ID)
   - MJPEG container writer (video + audio sync)

3. **Social Media Service** (`svc_social.c`)
   - OAuth2 token management (encrypted storage in NVS)
   - Platform-specific API adapters (Instagram, X, Facebook)
   - Multipart upload with progress callback
   - Background upload queue

4. **Gallery Service** (`svc_gallery.c`)
   - Thumbnail generation (downscale JPEG to 80×80)
   - Media file indexing
   - Metadata extraction (date, size, dimensions)

### App Screens

1. **Camera Viewfinder** (Main)
   - Live preview (downscaled for display)
   - Shutter button (tap = photo, hold = video)
   - Mode toggle (Photo/Video)
   - Gallery icon (bottom-left)
   - Settings icon (top-right)

2. **Gallery**
   - Grid of thumbnails (3×4)
   - Tap to view full-screen
   - Swipe to navigate
   - Share button → Platform selection dialog
   - Delete button

3. **Settings**
   - Resolution (QVGA/VGA/SVGA/XGA/UXGA)
   - JPEG quality (10-63)
   - White balance (Auto/Sunny/Cloudy/Office/Home)
   - Exposure compensation (-2 to +2)
   - Effects (None/Negative/BW/Red/Green/Blue/Retro)
   - Timestamp overlay (on/off)
   - Social accounts (connect/disconnect Instagram, X, Facebook)

4. **Social Share Dialog**
   - Platform selection (icons)
   - Caption input (voice or on-screen keyboard)
   - Hashtag suggestions
   - Post button → Background upload queue

### Kernel API Extensions (ABI v1.7)

```c
/* Camera API */
typedef struct {
    k_err_t (*init)(int width, int height, int jpeg_quality);
    k_err_t (*capture_photo)(const char* path);
    k_err_t (*start_video)(const char* path, int fps);
    k_err_t (*stop_video)(void);
    k_err_t (*get_preview_frame)(uint8_t* rgb565_buf);
    k_err_t (*set_resolution)(int width, int height);
    k_err_t (*set_quality)(int quality);
    k_err_t (*set_effect)(int effect_id);
} k_camera_api_t;

/* Social Media API */
typedef struct {
    k_err_t (*connect_oauth)(const char* platform, const char* auth_url);
    k_err_t (*upload_photo)(const char* platform, const char* path, const char* caption, void (*progress_cb)(int percent));
    k_err_t (*upload_video)(const char* platform, const char* path, const char* caption, void (*progress_cb)(int percent));
    bool (*is_connected)(const char* platform);
    k_err_t (*disconnect)(const char* platform);
} k_social_api_t;

/* Gallery API */
typedef struct {
    k_err_t (*scan_media)(void);
    int (*get_media_count)(void);
    k_err_t (*get_media_info)(int index, char* out_path, char* out_date, int* out_size);
    k_err_t (*generate_thumbnail)(const char* path, uint8_t* rgb565_buf, int width, int height);
    k_err_t (*delete_media)(const char* path);
} k_gallery_api_t;
```

---

## Hardware Pinout: OV2640 DVP Camera

```
ESP32-S3 GPIO    OV2640 Pin    Function
─────────────────────────────────────────
GPIO 15          D0            Data bit 0
GPIO 17          D1            Data bit 1
GPIO 18          D2            Data bit 2
GPIO 16          D3            Data bit 3
GPIO 14          D4            Data bit 4
GPIO 12          D5            Data bit 5
GPIO 11          D6            Data bit 6
GPIO 48          D7            Data bit 7
GPIO 13          VSYNC         Vertical sync
GPIO 38          HREF          Horizontal reference
GPIO 10          PCLK          Pixel clock
GPIO 40          XCLK          Master clock (20MHz output)
GPIO 39          SDA           SCCB (I2C) Data
GPIO 41          SCL           SCCB (I2C) Clock
GPIO 21          RESET         Active low reset
GPIO 47          PWDN          Power down (active high)
```

---

## 1. Camera Service (`svc_camera.h/.c`)

This service manages the OV2640 hardware, framebuffers in PSRAM, and provides capture APIs.


## 2. Gallery Service (`svc_gallery.h/.c`)

Manages media files, generates thumbnails, and provides indexing.

## 3. Social Media Service (`svc_social.h/.c`)

Handles Telegram Bot API integration with OAuth token storage.

## 4. Camera App (`app_camera.c`)

The complete user-facing application with viewfinder, gallery, settings, and voice commands.

## 5. Kernel API Extensions (ABI v1.7)

Update `kernel_api.h`:

```c
/* Add new API sub-tables */
typedef struct {
    k_err_t (*init)(void);
    k_err_t (*capture_jpeg)(const char* path, int quality);
    k_err_t (*start_video)(const char* path, int fps);
    k_err_t (*stop_video)(void);
    k_err_t (*get_preview_rgb565)(uint8_t* buf, int width, int height);
    k_err_t (*set_resolution)(int res);
    k_err_t (*set_quality)(int quality);
    k_err_t (*set_effect)(int effect);
    bool (*is_recording)(void);
} k_camera_api_t;

typedef struct {
    k_err_t (*scan_media)(void);
    int (*get_count)(void);
    k_err_t (*get_info)(int index, void* out_info);
    k_err_t (*generate_thumbnail)(const char* path, uint8_t* buf, int w, int h);
    k_err_t (*delete)(const char* path);
} k_gallery_api_t;

typedef struct {
    k_err_t (*init)(void);
    bool (*is_connected)(int platform);
    k_err_t (*connect_telegram)(const char* bot_token, const char* chat_id);
    k_err_t (*disconnect)(int platform);
    k_err_t (*upload_photo)(int platform, const char* path, const char* caption, void (*progress_cb)(int));
} k_social_api_t;

/* Add to KernelAPI struct (append only): */
typedef struct {
    // ... existing fields ...
    k_camera_api_t camera;
    k_gallery_api_t gallery;
    k_social_api_t social;
} KernelAPI;
```

Wire in `kernel_main.c`:

```c
.camera = {
    .init = svc_camera_init,
    .capture_jpeg = svc_camera_capture_jpeg,
    .start_video = svc_camera_start_video,
    .stop_video = svc_camera_stop_video,
    .get_preview_rgb565 = svc_camera_get_preview_rgb565,
    .set_resolution = (void*)svc_camera_set_resolution,
    .set_quality = svc_camera_set_quality,
    .set_effect = (void*)svc_camera_set_effect,
    .is_recording = svc_camera_is_recording,
},
.gallery = {
    .scan_media = svc_gallery_init,
    .get_count = svc_gallery_get_count,
    .get_info = (void*)svc_gallery_get_info,
    .generate_thumbnail = svc_gallery_generate_thumbnail,
    .delete = svc_gallery_delete,
},
.social = {
    .init = svc_social_init,
    .is_connected = svc_social_is_connected,
    .connect_telegram = svc_social_connect_telegram,
    .disconnect = svc_social_disconnect,
    .upload_photo = svc_social_upload_photo,
},
```

Add to kernel's `idf_component.yml`:

```yaml
dependencies:
  espressif/esp32-camera: "^2.0.0"
```

---

## 7. Deployment & Testing

### Build & Deploy

```bash
# 1. Build kernel with camera support
cd esp-appos
idf.py build flash monitor

# 2. Build camera app
cd apps/camera
mkdir build && cd build
cmake .. && make build_espapp

# 3. Copy to SD card
cp build/camera.espapp /mnt/sd/apps/
echo '{"name":"Camera","build":1}' > /mnt/sd/apps/camera.manifest.json
mkdir -p /mnt/sd/media
```

### Telegram Setup

1. Create a Telegram bot via [@BotFather](https://t.me/BotFather)
2. Get your Bot Token (e.g., `123456:ABC-DEF1234ghIkl-zyx57W2v1u123ew11`)
3. Get your Chat ID by messaging [@userinfobot](https://t.me/userinfobot)
4. In the Camera app Settings screen, tap "Connect Telegram"
5. Enter Bot Token and Chat ID (via voice or on-screen keyboard)

### Voice Commands

After wake word ("Hey ESP"):
- "Take picture" → Captures photo to `/media/`
- "Record video" → Starts/stops MJPEG recording
- "Open gallery" → Shows photo grid
- "Post to Telegram" → Uploads last photo

---

## Video Audio Sync Implementation (`svc_av_recorder.h/c`) and A/V Recorder (`svc_av_recorder.c`)

The synchronized A/V recorder service that captures MJPEG video frames and PCM audio together in a simple container format. This will handle timestamp synchronization, buffering, and file I/O in a dedicated background task.

A simple custom container format **ESPAV** (ESP Audio Video) specification:
[ESPAV_SPECS.md](ESPAV_SPECS.md)

A Python tool to convert `.espav` files to standard formats
[/tools/espav_converter.py](/tools/espav_converter.py)

### Kernel API Extension

Update `kernel_api.h` to expose the A/V recorder:

```c
typedef struct {
    k_err_t (*init)(void);
    k_err_t (*start)(const char* path, int video_fps, int audio_sample_rate, int audio_channels, int audio_bits);
    k_err_t (*stop)(void);
    int (*get_state)(void);
    uint32_t (*get_duration_ms)(void);
    uint32_t (*get_frame_count)(void);
} k_av_recorder_api_t;

// Add to KernelAPI struct (append only):
typedef struct {
    // ... existing fields ...
    k_av_recorder_api_t av_recorder;
} KernelAPI;
```

Wire in `kernel_main.c`:

```c
.av_recorder = {
    .init = svc_av_recorder_init,
    .start = (k_err_t (*)(const char*, int, int, int, int))svc_av_recorder_start_wrapper,
    .stop = svc_av_recorder_stop,
    .get_state = (int (*)(void))svc_av_recorder_get_state,
    .get_duration_ms = svc_av_recorder_get_duration_ms,
    .get_frame_count = svc_av_recorder_get_frame_count,
},
```

Add wrapper function in `kernel_main.c`:

```c
static k_err_t svc_av_recorder_start_wrapper(const char* path, int video_fps, int audio_sample_rate, int audio_channels, int audio_bits) {
    av_recorder_config_t config = {
        .video_fps = video_fps,
        .audio_sample_rate = audio_sample_rate,
        .audio_channels = audio_channels,
        .audio_bits = audio_bits,
    };
    return svc_av_recorder_start(path, &config);
}
```

### Camera App Integration

Update `app_camera.c` to use the synchronized recorder:

```c
static void toggle_video_recording(void) {
    if (g_api->av_recorder.get_state() == 1) { // RECORDING
        g_api->av_recorder.stop();
        g_api->display.obj_set_prop(g_btn_shutter, "text", "● Photo");
        g_api->display.obj_set_prop(g_lbl_status, "text", "Video saved");
    } else {
        time_t now;
        time(&now);
        struct tm tm;
        localtime_r(&now, &tm);

        char path[128];
        snprintf(path, sizeof(path), "/sdcard/media/%04d%02d%02d_%02d%02d%02d.espav",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);

        k_err_t res = g_api->av_recorder.start(path, 15, 16000, 1, 16);
        
        if (res == K_OK) {
            g_api->display.obj_set_prop(g_btn_shutter, "text", "■ Stop");
            g_api->display.obj_set_prop(g_lbl_status, "text", "Recording...");
        } else {
            g_api->display.obj_set_prop(g_lbl_status, "text", "Record failed");
        }
    }
}

// Add periodic UI update in app_main loop
static void update_recording_status(void* arg) {
    if (g_api->av_recorder.get_state() == 1) {
        char status[64];
        snprintf(status, sizeof(status), "Recording: %us | %u frames",
                 g_api->av_recorder.get_duration_ms() / 1000,
                 g_api->av_recorder.get_frame_count());
        g_api->display.obj_set_prop(g_lbl_status, "text", status);
    }
}
```

### Build Configuration

Add to kernel's `CMakeLists.txt`:

```cmake
# Add A/V recorder service
set(SRC_FILES
    ${SRC_FILES}
    "services/svc_av_recorder.c"
)
```

---

### Build & Deploy
```bash
# Rebuild kernel with A/V recorder
idf.py build flash monitor

# Rebuild camera app
cd apps/camera/build
cmake .. && make build_espapp

# Deploy to SD card
cp camera.espapp /mnt/sd/apps/
```

### Test Procedure
1. Launch Camera app
2. Tap shutter button to start video recording
3. Record for 10-30 seconds (speak into microphone)
4. Tap shutter button again to stop
5. Copy `.espav` file from SD card to PC
6. Run converter: `python espav_converter.py video.espav output`
7. Combine with ffmpeg: `ffmpeg -i output.mjpeg -i output.wav -c:v copy -c:a aac video.mp4`
8. Play `video.mp4` - verify audio/video sync














---

## Validation Matrix

| Test | Expected Result |
|:-----|:----------------|
| Photo capture | JPEG saved to `/media/YYYYMMDD_HHMMSS.jpg`, ~50-100KB |
| Video recording | MJPEG file created, ~5MB for 30s VGA video |
| Gallery browsing | Thumbnails displayed, tap to view full-screen |
| Telegram upload | Photo appears in Telegram chat within 5s |
| Voice command "take picture" | Photo captured without touching screen |
| Settings persistence | Resolution/quality saved to NVS, survives reboot |
| SD card full | Graceful error message, no crash |
| Network dropout during upload | Retry logic, progress indicator |
| Start recording | Status shows "Recording: 0s \| 0 frames" |
| Record 10s video | File size ~2MB, 150 frames, audio chunks present |
| Stop recording | File finalized, status shows "Video saved" |
| Playback sync | Audio matches video within ±100ms |
| Long recording (5min) | No dropped frames, stable memory usage |

---

## TODO:

1. **Add live preview** - Stream camera frames to LVGL image widget at 10fps
2. ✅ **Implement video audio sync** - Record microphone audio alongside MJPEG frames
3. **Add more social platforms** - X/Twitter OAuth, Instagram Graph API
4. **Photo editing** - Crop, filters, text overlay before sharing
5. **Cloud backup** - Auto-upload to Google Drive/Dropbox
6. **Implement video thumbnail generation** - Extract first frame for gallery
7. **Add video playback in gallery** - Play `.espav` files on device
8. **Optimize for larger files** - Add file size limits, auto-split at 100MB
