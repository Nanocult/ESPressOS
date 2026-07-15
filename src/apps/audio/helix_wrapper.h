/**
 * @file helix_wrapper.h
 * @brief Minimal abstraction over libhelix MP3 decoder for ESP-AppOS
 */
#ifndef HELIX_WRAPPER_H
#define HELIX_WRAPPER_H

#include <stdint.h>

typedef void* helix_decoder_t;

typedef struct {
    int sample_rate;  /* e.g., 44100, 48000, 32000 */
    int channels;     /* 1 = Mono, 2 = Stereo */
} helix_info_t;

/** Allocate decoder state (requires ~30KB from PSRAM pool) */
helix_decoder_t helix_create(void);

/** Free decoder state */
void helix_destroy(helix_decoder_t dec);

/**
 * Decode MP3 frames.
 * @param dec           Decoder instance
 * @param in_buf        Input buffer containing MP3 data
 * @param in_len        Bytes available in in_buf
 * @param bytes_consumed [OUT] How many bytes Helix processed (app must advance buffer)
 * @param out_pcm       Output buffer for 16-bit PCM samples
 * @param max_out_samples Max PCM samples the output buffer can hold
 * @param out_info      [OUT] Populated with sample rate/channels of decoded frame
 * @return Number of PCM samples generated, or < 0 on error
 */
int helix_decode(helix_decoder_t dec, const uint8_t* in_buf, int in_len, 
                 int* bytes_consumed, int16_t* out_pcm, int max_out_samples, 
                 helix_info_t* out_info);

#endif
