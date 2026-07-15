#ifndef OPUS_WRAPPER_H
#define OPUS_WRAPPER_H
#include <stdint.h>
#include <stddef.h>

typedef void* opus_encoder_t;
typedef void* opus_decoder_t;

#define OPUS_APP_VOIP 2048

opus_encoder_t opus_encoder_create(int fs, int channels, int application);
void opus_encoder_destroy(opus_encoder_t enc);
int opus_encode(opus_encoder_t enc, const int16_t* pcm, int frame_size, uint8_t* out, int max_out);

opus_decoder_t opus_decoder_create(int fs, int channels);
void opus_decoder_destroy(opus_decoder_t dec);
int opus_decode(opus_decoder_t dec, const uint8_t* in, int len, int16_t* pcm, int frame_size);
#endif
