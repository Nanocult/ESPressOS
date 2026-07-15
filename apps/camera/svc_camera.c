#include "svc_camera.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "driver/ledc.h"
#include <stdio.h>
#include <string.h>

static const char* TAG = "svc_camera";
static bool s_initialized = false;
static bool s_recording = false;
static FILE* s_video_file = NULL;

/* OV2640 Pin Configuration */
#define CAM_PIN_D0      15
#define CAM_PIN_D1      17
#define CAM_PIN_D2      18
#define CAM_PIN_D3      16
#define CAM_PIN_D4      14
#define CAM_PIN_D5      12
#define CAM_PIN_D6      11
#define CAM_PIN_D7      48
#define CAM_PIN_VSYNC   13
#define CAM_PIN_HREF    38
#define CAM_PIN_PCLK    10
#define CAM_PIN_XCLK    40
#define CAM_PIN_SDA     39
#define CAM_PIN_SCL     41
#define CAM_PIN_RESET   21
#define CAM_PIN_PWDN    47

k_err_t svc_camera_init(void) {
    if (s_initialized) return K_OK;

    camera_config_t config = {
        .pin_pwdn = CAM_PIN_PWDN,
        .pin_reset = CAM_PIN_RESET,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SDA,
        .pin_sccb_scl = CAM_PIN_SCL,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = 20000000, // 20MHz XCLK
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = FRAMESIZE_VGA, // Default VGA
        .jpeg_quality = 12, // 0-63, lower = higher quality
        .fb_count = 2, // Double buffering
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return K_ERR_IO;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "✓ Camera initialized (OV2640 VGA)");
    return K_OK;
}

k_err_t svc_camera_capture_jpeg(const char* path, int quality) {
    if (!s_initialized) return K_ERR_INVALID;

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture failed");
        return K_ERR_IO;
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        esp_camera_fb_return(fb);
        return K_ERR_IO;
    }

    fwrite(fb->buf, 1, fb->len, f);
    fclose(f);
    esp_camera_fb_return(fb);

    ESP_LOGI(TAG, "Photo saved: %s (%u bytes)", path, fb->len);
    return K_OK;
}

k_err_t svc_camera_start_video(const char* path, int fps) {
    if (!s_initialized || s_recording) return K_ERR_INVALID;

    s_video_file = fopen(path, "wb");
    if (!s_video_file) return K_ERR_IO;

    s_recording = true;
    ESP_LOGI(TAG, "Video recording started: %s @ %d fps", path, fps);
    
    /* TODO: Spawn video capture task that writes MJPEG frames */
    return K_OK;
}

k_err_t svc_camera_stop_video(void) {
    if (!s_recording) return K_OK;
    
    s_recording = false;
    if (s_video_file) {
        fclose(s_video_file);
        s_video_file = NULL;
    }
    ESP_LOGI(TAG, "Video recording stopped");
    return K_OK;
}

k_err_t svc_camera_get_preview_rgb565(uint8_t* buf, int width, int height) {
    if (!s_initialized) return K_ERR_INVALID;

    /* Get JPEG frame, decode to RGB565, downscale */
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return K_ERR_IO;

    /* For preview, we'd use a JPEG decoder + downscaler.
     * For MVP, return raw JPEG and let LVGL decode it. */
    memcpy(buf, fb->buf, fb->len < (width * height * 2) ? fb->len : (width * height * 2));
    
    esp_camera_fb_return(fb);
    return K_OK;
}

k_err_t svc_camera_set_resolution(cam_resolution_t res) {
    if (!s_initialized) return K_ERR_INVALID;
    
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return K_ERR_IO;

    framesize_t fs;
    switch (res) {
        case CAM_RES_QQVGA: fs = FRAMESIZE_QQVGA; break;
        case CAM_RES_QVGA:  fs = FRAMESIZE_QVGA; break;
        case CAM_RES_VGA:   fs = FRAMESIZE_VGA; break;
        case CAM_RES_SVGA:  fs = FRAMESIZE_SVGA; break;
        case CAM_RES_XGA:   fs = FRAMESIZE_XGA; break;
        case CAM_RES_UXGA:  fs = FRAMESIZE_UXGA; break;
        default: return K_ERR_INVALID;
    }

    s->set_framesize(s, fs);
    ESP_LOGI(TAG, "Resolution set to %d", res);
    return K_OK;
}

k_err_t svc_camera_set_quality(int quality) {
    if (!s_initialized || quality < 10 || quality > 63) return K_ERR_INVALID;
    
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return K_ERR_IO;
    
    s->set_quality(s, quality);
    return K_OK;
}

k_err_t svc_camera_set_effect(cam_effect_t effect) {
    if (!s_initialized) return K_ERR_INVALID;
    
    sensor_t* s = esp_camera_sensor_get();
    if (!s) return K_ERR_IO;

    int special_effect = (int)effect;
    s->set_special_effect(s, special_effect);
    return K_OK;
}

bool svc_camera_is_recording(void) {
    return s_recording;
}
