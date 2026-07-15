/**
 * @file app_voice.c
 * @brief ESP-AppOS Core App: Voice Assistant (Opus + WebSocket)
 */
#include "kernel_api.h"
#include "opus_wrapper.h"

static const KernelAPI* g_api;

/* ========================================================================== */
/* NOSTDLIB UTILS & CONSTANTS                                                 */
/* ========================================================================== */
static int my_strlen(const char* s) { int i = 0; while (s && s[i]) i++; return i; }
static void my_strcpy(char* dst, const char* src) { while (*src) *dst++ = *src++; *dst = '\0'; }

#define SAMPLE_RATE     16000
#define CHANNELS        1
#define FRAME_SIZE      320     /* 20ms at 16kHz */
#define MAX_PACKET      1024
#define K_EVT_PRESSED   2
#define K_EVT_RELEASED  4

/* ========================================================================== */
/* APP STATE                                                                  */
/* ========================================================================== */
static k_thread_t g_stream_thread = NULL;
static volatile bool g_is_recording = false;
static k_audio_handle_t g_tts_out = NULL;

static k_lvgl_obj_t g_lbl_status;
static k_lvgl_obj_t g_btn_talk;

/* ========================================================================== */
/* STREAMING THREAD                                                           */
/* Runs independently of LVGL. Handles Mic -> Opus -> WS -> Opus -> Speaker   */
/* ========================================================================== */
static void stream_thread_fn(void* arg) {
    /* 1. Setup Audio I/O */
    k_audio_format_t mic_fmt = { .sample_rate = SAMPLE_RATE, .channels = CHANNELS, .bits_per_sample = 16, .codec = 0 };
    k_audio_handle_t mic;
    g_api->audio.mic_open(&mic_fmt, &mic);
    
    k_audio_format_t spk_fmt = { .sample_rate = SAMPLE_RATE, .channels = CHANNELS, .bits_per_sample = 16, .codec = 0 };
    g_api->audio.open_output(&spk_fmt, &g_tts_out);

    /* 2. Setup Network & Codecs */
    k_ws_handle_t ws;
    g_api->net.ws.open("wss://api.yourserver.com/v1/voice", &ws);
    
    opus_encoder_t enc = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APP_VOIP);
    opus_decoder_t dec = opus_decoder_create(SAMPLE_RATE, CHANNELS);

    int16_t* pcm_buf = g_api->mem.malloc(FRAME_SIZE * sizeof(int16_t));
    uint8_t* net_buf = g_api->mem.malloc(MAX_PACKET);

    /* 3. Main Streaming Loop */
    while (g_is_recording) {
        /* Capture Mic */
        size_t read_bytes = 0;
        g_api->audio.mic_read(mic, pcm_buf, FRAME_SIZE * sizeof(int16_t), &read_bytes, 100);
        
        if (read_bytes > 0) {
            int opus_len = opus_encode(enc, pcm_buf, read_bytes / sizeof(int16_t), net_buf, MAX_PACKET);
            if (opus_len > 0) {
                g_api->net.ws.send(ws, net_buf, opus_len, K_WS_MSG_BIN);
            }
        }

        /* Check for incoming TTS (Non-blocking) */
        size_t tts_len = 0;
        k_ws_msg_type_t msg_type;
        if (g_api->net.ws.recv(ws, net_buf, MAX_PACKET, &tts_len, &msg_type, 0) == K_OK && tts_len > 0) {
            if (msg_type == K_WS_MSG_BIN) {
                /* Decode Opus to PCM and play */
                int decoded_samples = opus_decode(dec, net_buf, tts_len, pcm_buf, FRAME_SIZE);
                if (decoded_samples > 0) {
                    g_api->audio.write(g_tts_out, pcm_buf, decoded_samples * sizeof(int16_t), 0);
                }
            } else {
                /* Text control message (e.g., {"status": "done"}) */
                if (my_strlen((char*)net_buf) > 10) { // Simplified check
                    g_is_recording = false; // Server finished speaking
                }
            }
        }
    }

    /* 4. Cleanup */
    g_api->mem.free(pcm_buf);
    g_api->mem.free(net_buf);
    opus_encoder_destroy(enc);
    opus_decoder_destroy(dec);
    g_api->net.ws.close(ws);
    g_api->audio.mic_close(mic);
    g_api->audio.close(g_tts_out);
    g_tts_out = NULL;
}

/* ========================================================================== */
/* UI EVENT HANDLERS                                                          */
/* ========================================================================== */
static void on_btn_event(k_lvgl_obj_t obj, uint32_t evt, void* ud) {
    if (evt == K_EVT_PRESSED && !g_is_recording) {
        g_is_recording = true;
        g_api->display.obj_set_prop(g_lbl_status, "text", "Listening...");
        g_api->sys.thread_create(stream_thread_fn, NULL, "voice_stream", 8192, &g_stream_thread);
    } 
    else if (evt == K_EVT_RELEASED && g_is_recording) {
        g_is_recording = false; /* Signals thread to stop sending mic data */
        g_api->display.obj_set_prop(g_lbl_status, "text", "Processing...");
        
        /* Wait for thread to finish playing TTS */
        g_api->sys.thread_join(g_stream_thread);
        g_stream_thread = NULL;
        
        g_api->display.obj_set_prop(g_lbl_status, "text", "Hold to Talk");
    }
}

/* ========================================================================== */
/* APP ENTRY                                                                  */
/* ========================================================================== */
void app_main(const KernelAPI* api) {
    g_api = api;
    api->display.init_screen(240, 320);

    g_lbl_status = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(g_lbl_status, "text", "Hold to Talk");
    api->display.obj_set_prop(g_lbl_status, "align", "top_mid");
    api->display.obj_set_prop(g_lbl_status, "y", "40");
    api->display.obj_set_prop(g_lbl_status, "text_font", "montserrat_24");

    g_btn_talk = api->display.obj_create("btn", NULL);
    api->display.obj_set_prop(g_btn_talk, "width", "160");
    api->display.obj_set_prop(g_btn_talk, "height", "160");
    api->display.obj_set_prop(g_btn_talk, "align", "center");
    
    k_lvgl_obj_t icon = api->display.obj_create("label", g_btn_talk);
    api->display.obj_set_prop(icon, "symbol", LV_SYMBOL_AUDIO);
    api->display.obj_set_prop(icon, "text_font", "montserrat_48");

    /* Subscribe to Press AND Release events */
    api->display.obj_on_event(g_btn_talk, K_EVT_PRESSED | K_EVT_RELEASED, on_btn_event, NULL);
    api->display.flush();
}

void app_deinit(void) {
    if (g_is_recording) {
        g_is_recording = false;
        if (g_stream_thread) g_api->sys.thread_join(g_stream_thread);
    }
    if (g_tts_out) g_api->audio.close(g_tts_out);
}
