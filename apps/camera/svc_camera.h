#ifndef SVC_CAMERA_H
#define SVC_CAMERA_H

#include "kernel_api.h"

typedef enum {
    CAM_RES_QQVGA,    // 160x120
    CAM_RES_QVGA,     // 320x240
    CAM_RES_VGA,      // 640x480
    CAM_RES_SVGA,     // 800x600
    CAM_RES_XGA,      // 1024x768
    CAM_RES_UXGA,     // 1600x1200
} cam_resolution_t;

typedef enum {
    CAM_EFFECT_NONE,
    CAM_EFFECT_NEGATIVE,
    CAM_EFFECT_BW,
    CAM_EFFECT_RED,
    CAM_EFFECT_GREEN,
    CAM_EFFECT_BLUE,
    CAM_EFFECT_RETRO,
} cam_effect_t;

k_err_t svc_camera_init(void);
k_err_t svc_camera_capture_jpeg(const char* path, int quality);
k_err_t svc_camera_start_video(const char* path, int fps);
k_err_t svc_camera_stop_video(void);
k_err_t svc_camera_get_preview_rgb565(uint8_t* buf, int width, int height);
k_err_t svc_camera_set_resolution(cam_resolution_t res);
k_err_t svc_camera_set_quality(int quality); // 10-63
k_err_t svc_camera_set_effect(cam_effect_t effect);
bool svc_camera_is_recording(void);

#endif
