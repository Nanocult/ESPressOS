/**
 * @file app_audio.c
 * @brief ESP-AppOS Core App: Universal Audio Player (WAV + MP3)
 */
#include "kernel_api.h"
#include "helix_wrapper.h"

static const KernelAPI* g_api;

/* ========================================================================== */
/* NOSTDLIB UTILS                                                             */
/* ========================================================================== */
static int my_strlen(const char* s) { int i = 0; while (s && s[i]) i++; return i; }
static int my_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char*)a - *(unsigned char*)b;
}
static void my_strcpy(char* dst, const char* src) { while (*src) *dst++ = *src++; *dst = '\0'; }
static void my_strcat(char* dst, const char* src) { while (*dst) dst++; while (*src) *dst++ = *src++; *dst = '\0'; }
static int my_str_ends_with(const char* str, const char* suffix) {
    int str_len = my_strlen(str);
    int suffix_len = my_strlen(suffix);
    if (suffix_len > str_len) return 0;
    return my_strcmp(str + str_len - suffix_len, suffix) == 0;
}

/* ========================================================================== */
/* APP STATE                                                                  */
/* ========================================================================== */
#define MAX_FILES       8
#define READ_BUF_SIZE   2048    /* SD read chunk */
#define PCM_BUF_SIZE    4096    /* PCM output buffer (16-bit samples) */
#define FILE_TYPE_WAV   1
#define FILE_TYPE_MP3   2

typedef struct {
    char name[32];
    int  type;
} media_file_t;

static media_file_t g_files[MAX_FILES];
static int g_file_count = 0;
static int g_current_idx = -1;

static volatile bool g_is_playing = false;
static k_thread_t g_playback_thread = NULL;
static k_lvgl_obj_t g_lbl_status;
static k_lvgl_obj_t g_btn_play;

/* ========================================================================== */
/* WAV HEADER PARSER                                                          */
/* ========================================================================== */
typedef struct __attribute__((packed)) {
    uint32_t chunk_id;
    uint32_t chunk_size;
    uint32_t format;
    uint32_t subchunk1_id;
    uint32_t subchunk1_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_fmt_t;

static bool parse_wav_header(k_file_t f, k_audio_format_t* out_fmt) {
    uint8_t hdr[44];
    size_t read = 0;
    if (g_api->fs.read(f, hdr, 44, &read) != K_OK || read < 44) return false;
    
    wav_fmt_t* fmt = (wav_fmt_t*)hdr;
    if (fmt->chunk_id != 0x46464952) return false; /* "RIFF" */
    if (fmt->format != 0x45564157) return false;   /* "WAVE" */
    
    out_fmt->sample_rate = fmt->sample_rate;
    out_fmt->channels = fmt->num_channels;
    out_fmt->bits_per_sample = fmt->bits_per_sample;
    out_fmt->codec = 0; /* PCM */
    return true;
}

/* ========================================================================== */
/* PLAYBACK THREAD (Handles both WAV and MP3)                                 */
/* ========================================================================== */
static void playback_thread_fn(void* arg) {
    char path[64];
    my_strcpy(path, "/media/");
    my_strcat(path, g_files[g_current_idx].name);
    
    k_file_t file = NULL;
    k_audio_handle_t audio_out = NULL;
    helix_decoder_t helix_dec = NULL;
    uint8_t* read_buf = g_api->mem.malloc(READ_BUF_SIZE);
    int16_t* pcm_buf = g_api->mem.malloc(PCM_BUF_SIZE * sizeof(int16_t));
    
    int file_type = g_files[g_current_idx].type;
    int read_buf_len = 0;
    int read_buf_offset = 0;

    if (g_api->fs.open(path, K_FILE_READ, &file) != K_OK) goto cleanup;

    /* 1. Configure Audio Output based on file type */
    k_audio_format_t fmt = {0};
    
    if (file_type == FILE_TYPE_WAV) {
        if (!parse_wav_header(file, &fmt)) {
            g_api->sys.log(0, "Audio", "Invalid WAV header");
            goto cleanup;
        }
    } 
    else if (file_type == FILE_TYPE_MP3) {
        helix_dec = helix_create();
        if (!helix_dec) {
            g_api->sys.log(0, "Audio", "Helix alloc failed");
            goto cleanup;
        }
        
        /* Read first chunk to probe MP3 format */
        size_t r = 0;
        g_api->fs.read(file, read_buf, READ_BUF_SIZE, &r);
        read_buf_len = r;
        
        int consumed = 0;
        helix_info_t info;
        /* Decode 1 frame just to get sample rate/channels */
        helix_decode(helix_dec, read_buf, read_buf_len, &consumed, pcm_buf, PCM_BUF_SIZE, &info);
        
        fmt.sample_rate = info.sample_rate;
        fmt.channels = info.channels;
        fmt.bits_per_sample = 16;
        fmt.codec = 0;
        
        /* Reset file to start (Helix doesn't support seeking easily, so re-open) */
        g_api->fs.close(file);
        g_api->fs.open(path, K_FILE_READ, &file);
    }

    if (g_api->audio.open_output(&fmt, &audio_out) != K_OK) {
        g_api->sys.log(0, "Audio", "Audio out open failed");
        goto cleanup;
    }

    /* 2. Main Decoding Loop */
    while (g_is_playing) {
        if (file_type == FILE_TYPE_WAV) {
            size_t r = 0;
            if (g_api->fs.read(file, read_buf, READ_BUF_SIZE, &r) != K_OK || r == 0) break;
            /* Blocking write with 100ms timeout. Safe because we are in a background thread. */
            g_api->audio.write(audio_out, read_buf, r, 100); 
        } 
        else if (file_type == FILE_TYPE_MP3) {
            /* Refill read buffer if needed */
            if (read_buf_offset > 0) {
                /* Move unconsumed data to front */
                int remaining = read_buf_len - read_buf_offset;
                for(int i=0; i<remaining; i++) read_buf[i] = read_buf[read_buf_offset + i];
                read_buf_len = remaining;
                read_buf_offset = 0;
            }
            if (read_buf_len < READ_BUF_SIZE / 2) {
                size_t r = 0;
                if (g_api->fs.read(file, read_buf + read_buf_len, READ_BUF_SIZE - read_buf_len, &r) != K_OK || r == 0) {
                    if (read_buf_len == 0) break; /* True EOF */
                }
                read_buf_len += r;
            }

            int consumed = 0;
            helix_info_t info;
            int samples = helix_decode(helix_dec, read_buf, read_buf_len, &consumed, pcm_buf, PCM_BUF_SIZE, &info);
            
            if (consumed > 0) read_buf_offset += consumed;
            
            if (samples > 0) {
                g_api->audio.write(audio_out, pcm_buf, samples * sizeof(int16_t), 100);
            }
        }
    }

cleanup:
    g_is_playing = false;
    if (audio_out) g_api->audio.close(audio_out);
    if (file) g_api->fs.close(file);
    if (helix_dec) helix_destroy(helix_dec);
    if (read_buf) g_api->mem.free(read_buf);
    if (pcm_buf) g_api->mem.free(pcm_buf);
    
    /* Update UI safely (Kernel API calls are thread-safe) */
    g_api->display.obj_set_prop(g_lbl_status, "text", "Stopped");
    g_api->display.obj_set_prop(g_btn_play, "text", "Play");
}

/* ========================================================================== */
/* UI EVENT HANDLERS                                                          */
/* ========================================================================== */
static void on_file_selected(k_lvgl_obj_t obj, uint32_t evt, void* ud) {
    if (evt != 1) return;
    int idx = (int)(intptr_t)ud;
    
    if (g_is_playing) {
        g_is_playing = false;
        g_api->sys.thread_join(g_playback_thread);
        g_playback_thread = NULL;
    }
    
    g_current_idx = idx;
    char status[48];
    my_strcpy(status, "Ready: ");
    my_strcat(status, g_files[idx].name);
    g_api->display.obj_set_prop(g_lbl_status, "text", status);
    g_api->display.obj_set_prop(g_btn_play, "text", "Play");
}

static void on_play_pause(k_lvgl_obj_t obj, uint32_t evt, void* ud) {
    if (evt != 1) return;
    
    if (g_is_playing) {
        g_is_playing = false;
        g_api->sys.thread_join(g_playback_thread);
        g_playback_thread = NULL;
        g_api->display.obj_set_prop(g_btn_play, "text", "Play");
        g_api->display.obj_set_prop(g_lbl_status, "text", "Paused");
    } else {
        if (g_current_idx < 0) return;
        g_is_playing = true;
        g_api->sys.thread_create(playback_thread_fn, NULL, "audio_dec", 8192, &g_playback_thread);
        g_api->display.obj_set_prop(g_btn_play, "text", "Stop");
        g_api->display.obj_set_prop(g_lbl_status, "text", "Playing...");
    }
}

/* ========================================================================== */
/* DIRECTORY SCANNER & APP ENTRY                                              */
/* ========================================================================== */
static void scan_media_dir(void) {
    k_dir_t dir;
    if (g_api->fs.opendir("/media", &dir) != K_OK) return;
    
    char fname[64];
    bool is_dir;
    while (g_api->fs.readdir(dir, fname, sizeof(fname), &is_dir) == K_OK && g_file_count < MAX_FILES) {
        if (!is_dir) {
            int type = 0;
            if (my_str_ends_with(fname, ".wav")) type = FILE_TYPE_WAV;
            else if (my_str_ends_with(fname, ".mp3")) type = FILE_TYPE_MP3;
            
            if (type > 0) {
                my_strcpy(g_files[g_file_count].name, fname);
                g_files[g_file_count].type = type;
                g_file_count++;
            }
        }
    }
    g_api->fs.closedir(dir);
}

void app_main(const KernelAPI* api) {
    g_api = api;
    api->display.init_screen(240, 320);
    
    /* Header */
    k_lvgl_obj_t header = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(header, "text", "Media Player");
    api->display.obj_set_prop(header, "align", "top_mid");
    api->display.obj_set_prop(header, "y", "10");
    api->display.obj_set_prop(header, "text_font", "montserrat_24");
    
    /* List Container */
    k_lvgl_obj_t list = api->display.obj_create("obj", NULL);
    api->display.obj_set_prop(list, "width", "220");
    api->display.obj_set_prop(list, "height", "160");
    api->display.obj_set_prop(list, "align", "top_mid");
    api->display.obj_set_prop(list, "y", "50");
    api->display.obj_set_prop(list, "layout", "flex_col");
    api->display.obj_set_prop(list, "pad_all", "5");
    
    scan_media_dir();
    for (int i = 0; i < g_file_count; i++) {
        k_lvgl_obj_t btn = api->display.obj_create("btn", list);
        api->display.obj_set_prop(btn, "width", "200");
        api->display.obj_set_prop(btn, "height", "35");
        
        k_lvgl_obj_t lbl = api->display.obj_create("label", btn);
        char display_name[40];
        my_strcpy(display_name, g_files[i].type == FILE_TYPE_MP3 ? "[MP3] " : "[WAV] ");
        my_strcat(display_name, g_files[i].name);
        api->display.obj_set_prop(lbl, "text", display_name);
        
        api->display.obj_on_event(btn, 1, on_file_selected, (void*)(intptr_t)i);
    }

    g_lbl_status = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(g_lbl_status, "text", "Select track");
    api->display.obj_set_prop(g_lbl_status, "align", "bottom_mid");
    api->display.obj_set_prop(g_lbl_status, "y", "-80");
    
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
    if (g_is_playing) {
        g_is_playing = false;
        if (g_playback_thread) g_api->sys.thread_join(g_playback_thread);
    }
}
