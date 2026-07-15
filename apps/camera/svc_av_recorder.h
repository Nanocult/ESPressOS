#ifndef SVC_AV_RECORDER_H
#define SVC_AV_RECORDER_H

#include "kernel_api.h"
#include <stdbool.h>

typedef enum {
    AV_RECORDER_IDLE,
    AV_RECORDER_RECORDING,
    AV_RECORDER_STOPPING,
    AV_RECORDER_ERROR,
} av_recorder_state_t;

typedef struct {
    int video_fps;           // Target video frame rate (10-30)
    int audio_sample_rate;   // Audio sample rate (16000, 44100)
    int audio_channels;      // 1 = mono, 2 = stereo
    int audio_bits;          // 16 = 16-bit PCM
} av_recorder_config_t;

/**
 * Initialize A/V recorder service
 */
k_err_t svc_av_recorder_init(void);

/**
 * Start recording synchronized video + audio
 * @param path Output file path (e.g., "/sdcard/media/video.espav")
 * @param config Recording configuration
 */
k_err_t svc_av_recorder_start(const char* path, const av_recorder_config_t* config);

/**
 * Stop recording and finalize file
 */
k_err_t svc_av_recorder_stop(void);

/**
 * Get current recorder state
 */
av_recorder_state_t svc_av_recorder_get_state(void);

/**
 * Get recording duration in milliseconds
 */
uint32_t svc_av_recorder_get_duration_ms(void);

/**
 * Get number of video frames recorded
 */
uint32_t svc_av_recorder_get_frame_count(void);

#endif
