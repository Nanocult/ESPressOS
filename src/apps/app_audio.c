/**
 * @file app_audio.c
 * @brief ESP-AppOS Core App: Audio Player (WAV PCM Streaming)
 */
#include "kernel_api.h"

static const KernelAPI* g_api;

/* ========================================================================== */
/* NOSTDLIB STRING/MEMORY UTILS                                               */
/* ========================================================================== */
static int my_strlen(const char* s) { int i = 0; while (s && s[i]) i++; return i; }
static void my_strcpy(char* dst, const char* src) {
    if (!dst || !src) return;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}
static void my_strcat(char* dst, const char* src) {
    if (!dst || !src) return;
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

/* ========================================================================== */
/* APP STATE                                                                  */
/* ========================================================================== */
#define MAX_FILES 4
#define STREAM_BUF_SIZE 2048  /* 2KB chunks = ~11ms of 44.1kHz 16-bit stereo */

static char g_media_files[MAX_FILES][32];
static int g_file_count = 0;
static int g_current_file_idx = -1;

static uint8_t g_stream_buf[STREAM_BUF_SIZE];
static size_t  g_pending_bytes = 0;      /* Non-blocking write state */
static k_file_t g_audio_file = NULL;
static k_audio_handle_t g_audio_out = NULL;
static k_timer_t g_stream_timer = NULL;
static bool g_is_playing = false;

/* UI Elements */
static k_lvgl_obj_t g_lbl_status;
static k_lvgl_obj_t g_btn_play;
static k_lvgl_obj_t g_file_btns[MAX_FILES];

/* ========================================================================== */
/* WAV PARSER & PLAYBACK LOGIC                                                */
/* ========================================================================== */
static bool skip_wav_header(k_file_t f) {
    uint8_t hdr[44];
    size_t read = 0;
    if (g_api->fs.read(f, hdr, 44, &read) != K_OK || read != 44) return false;
    /* Validate RIFF and WAVE magic bytes */
    if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F') return false;
    if (hdr[8] != 'W' || hdr[9] != 'A' || hdr[10]!= 'V' || hdr[11]!= 'E') return false;
    return true;
}

static void stream_timer_cb(void* ud) {
    if (!g_is_playing || !g_audio_file || !g_audio_out) return;

    /* 1. Flush pending data if previous write failed (ringbuffer was full) */
    if (g_pending_bytes > 0) {
        if (g_api->audio.write(g_audio_out, g_stream_buf, g_pending_bytes, 0) == K_OK) {
            g_pending_bytes = 0;
        } else {
            return; /* Still full, try again next tick. UI is NOT blocked. */
        }
    }

    /* 2. Read next chunk from SD card */
    size_t bytes_read = 0;
    k_err_t res = g_api->fs.read(g_audio_file, g_stream_buf, STREAM_BUF_SIZE, &bytes_read);
    
    if (res != K_OK || bytes_read == 0) {
        /* EOF or Error - Stop playback */
        g_is_playing = false;
        g_api->sys.timer_delete(g_stream_timer);
        g_stream_timer = NULL;
        g_api->audio.close(g_audio_out);
        g_api->fs.close(g_audio_file);
        g_audio_file = NULL;
        g_api->display.obj_set_prop(g_lbl_status, "text", "Finished");
        g_api->display.obj_set_prop(g_btn_play, "text", "Play");
        return;
    }

    /* 3. Attempt zero-timeout write to audio engine */
    if (g_api->audio.write(g_audio_out, g_stream_buf, bytes_read, 0) != K_OK) {
        /* Ringbuffer full. Save chunk for next tick. DO NOT read again. */
        g_pending_bytes = bytes_read;
    }
}

/* ========================================================================== */
/* UI EVENT HANDLERS                                                          */
/* ========================================================================== */
static void on_play_pause(k_lvgl_obj_t obj, uint32_t evt, void* ud) {
    if (evt != 1) return; /* LV_EVENT_CLICKED */
    
    if (g_is_playing) {
        /* PAUSE */
        g_is_playing = false;
        if (g_stream_timer) {
            g_api->sys.timer_delete(g_stream_timer);
            g_stream_timer = NULL;
        }
        g_api->display.obj_set_prop(g_btn_play, "text", "Resume");
        g_api->display.obj_set_prop(g_lbl_status, "text", "Paused");
    } else {
        if (g_current_file_idx < 0) {
            g_api->display.obj_set_prop(g_lbl_status, "text", "Select a file");
            return;
        }
        
        /* PLAY / RESUME */
        if (!g_audio_file) {
            char path[64];
            my_strcpy(path, "/media/");
            my_strcat(path, g_media_files[g_current_file_idx]);
            
            if (g_api->fs.open(path, K_FILE_READ, &g_audio_file) != K_OK) {
                g_api->display.obj_set_prop(g_lbl_status, "text", "File error");
                return;
            }
            if (!skip_wav_header(g_audio_file)) {
                g_api->fs.close(g_audio_file);
                g_audio_file = NULL;
                g_api->display.obj_set_prop(g_lbl_status, "text", "Invalid WAV");
                return;
            }
            
            /* Open audio output (MVP hardcoded to 44.1kHz Stereo 16-bit) */
            k_audio_format_t fmt = { .sample_rate = 44100, .channels = 2, .bits_per_sample = 16, .codec = 0 };
            g_api->audio.open_output(&fmt, &g_audio_out);
        }
        
        g_is_playing = true;
        g_pending_bytes = 0;
        g_api->sys.timer_create(20, true, stream_timer_cb, NULL, &g_stream_timer);
        g_api->display.obj_set_prop(g_btn_play, "text", "Pause");
        g_api->display.obj_set_prop(g_lbl_status, "text", "Playing...");
    }
}

static void on_file_selected(k_lvgl_obj_t obj, uint32_t evt, void* ud) {
    if (evt != 1) return;
    int idx = (int)(intptr_t)ud;
    
    /* Stop current playback cleanly */
    if (g_is_playing) {
        g_is_playing = false;
        if (g_stream_timer) { g_api->sys.timer_delete(g_stream_timer); g_stream_timer = NULL; }
    }
    if (g_audio_out) { g_api->audio.close(g_audio_out); g_audio_out = NULL; }
    if (g_audio_file) { g_api->fs.close(g_audio_file); g_audio_file = NULL; }
    
    g_current_file_idx = idx;
    
    char status[48];
    my_strcpy(status, "Selected: ");
    my_strcat(status, g_media_files[idx]);
    g_api->display.obj_set_prop(g_lbl_status, "text", status);
    g_api->display.obj_set_prop(g_btn_play, "text", "Play");
}

/* ========================================================================== */
/* DIRECTORY SCANNER                                                          */
/* ========================================================================== */
static void scan_media_dir(void) {
    k_dir_t dir;
    if (g_api->fs.opendir("/media", &dir) != K_OK) {
        g_api->sys.log(1, "Audio", "/media dir not found");
        return;
    }
    
    char fname[64];
    bool is_dir;
    while (g_api->fs.readdir(dir, fname, sizeof(fname), &is_dir) == K_OK && g_file_count < MAX_FILES) {
        int len = my_strlen(fname);
        if (len > 4 && !is_dir) {
            if (fname[len-4] == '.' && fname[len-3] == 'w' && fname[len-2] == 'a' && fname[len-1] == 'v') {
                my_strcpy(g_media_files[g_file_count], fname);
                g_file_count++;
            }
        }
    }
    g_api->fs.closedir(dir);
}

/* ========================================================================== */
/* APP ENTRY & EXIT                                                           */
/* ========================================================================== */
void app_main(const KernelAPI* api) {
    g_api = api;
    if (api->abi_version < KERNEL_ABI_VERSION) {
        api->sys.request_exit(); return;
    }
    
    api->display.init_screen(240, 320);
    
    /* Header */
    k_lvgl_obj_t header = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(header, "text", "Audio Player");
    api->display.obj_set_prop(header, "align", "top_mid");
    api->display.obj_set_prop(header, "y", "10");
    api->display.obj_set_prop(header, "text_font", "montserrat_24");
    
    /* File List Container */
    k_lvgl_obj_t list_cont = api->display.obj_create("obj", NULL);
    api->display.obj_set_prop(list_cont, "width", "220");
    api->display.obj_set_prop(list_cont, "height", "160");
    api->display.obj_set_prop(list_cont, "align", "top_mid");
    api->display.obj_set_prop(list_cont, "y", "50");
    api->display.obj_set_prop(list_cont, "layout", "flex_col");
    api->display.obj_set_prop(list_cont, "pad_all", "5");
    
    scan_media_dir();
    for (int i = 0; i < g_file_count; i++) {
        g_file_btns[i] = api->display.obj_create("btn", list_cont);
        api->display.obj_set_prop(g_file_btns[i], "width", "200");
        api->display.obj_set_prop(g_file_btns[i], "height", "35");
        
        k_lvgl_obj_t lbl = api->display.obj_create("label", g_file_btns[i]);
        api->display.obj_set_prop(lbl, "text", g_media_files[i]);
        api->display.obj_set_prop(lbl, "text_font", "montserrat_14");
        
        api->display.obj_on_event(g_file_btns[i], 1, on_file_selected, (void*)(intptr_t)i);
    }
    
    if (g_file_count == 0) {
        k_lvgl_obj_t err = api->display.obj_create("label", list_cont);
        api->display.obj_set_prop(err, "text", "No .wav files in /media");
    }

    /* Status Label */
    g_lbl_status = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(g_lbl_status, "text", "Select a file");
    api->display.obj_set_prop(g_lbl_status, "align", "bottom_mid");
    api->display.obj_set_prop(g_lbl_status, "y", "-80");
    api->display.obj_set_prop(g_lbl_status, "text_font", "montserrat_16");
    
    /* Play/Pause Button */
    g_btn_play = api->display.obj_create("btn", NULL);
    api->display.obj_set_prop(g_btn_play, "width", "120");
    api->display.obj_set_prop(g_btn_play, "height", "50");
    api->display.obj_set_prop(g_btn_play, "align", "bottom_mid");
    api->display.obj_set_prop(g_btn_play, "y", "-20");
    
    k_lvgl_obj_t play_lbl = api->display.obj_create("label", g_btn_play);
    api->display.obj_set_prop(play_lbl, "text", "Play");
    api->display.obj_set_prop(play_lbl, "text_font", "montserrat_24");
    
    api->display.obj_on_event(g_btn_play, 1, on_play_pause, NULL);
    
    api->display.flush();
}

void app_deinit(void) {
    /* Guaranteed cleanup if kernel force-quits the app */
    if (g_is_playing && g_stream_timer) {
        g_api->sys.timer_delete(g_stream_timer);
    }
    if (g_audio_out) g_api->audio.close(g_audio_out);
    if (g_audio_file) g_api->fs.close(g_audio_file);
}
