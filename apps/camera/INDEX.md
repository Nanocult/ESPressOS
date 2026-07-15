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
