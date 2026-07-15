/**
 * @file app_camera.c
 * @brief ESP-AppOS Camera & Social Media App
 */
#include "kernel_api.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const KernelAPI* g_api;

/* ========================================================================== */
/* APP STATE                                                                  */
/* ========================================================================== */
typedef enum {
    SCREEN_VIEWFINDER,
    SCREEN_GALLERY,
    SCREEN_SETTINGS,
    SCREEN_SHARE,
} screen_t;

static screen_t g_current_screen = SCREEN_VIEWFINDER;
static int g_gallery_index = 0;
static char g_selected_photo[128] = {0};

/* UI Elements */
static k_lvgl_obj_t g_lbl_status;
static k_lvgl_obj_t g_img_preview;
static k_lvgl_obj_t g_btn_shutter;
static k_lvgl_obj_t g_btn_gallery;
static k_lvgl_obj_t g_btn_settings;
static k_lvgl_obj_t g_btn_share;

/* ========================================================================== */
/* CAMERA CAPTURE                                                             */
/* ========================================================================== */
static void capture_photo(void) {
    time_t now;
    time(&now);
    struct tm tm;
    localtime_r(&now, &tm);

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/media/%04d%02d%02d_%02d%02d%02d.jpg",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    g_api->display.obj_set_prop(g_lbl_status, "text", "Capturing...");
    g_api->display.flush();

    k_err_t res = g_api->camera.capture_jpeg(path, 12);
    if (res == K_OK) {
        g_api->display.obj_set_prop(g_lbl_status, "text", "Photo saved!");
        g_api->gallery.scan_media();
    } else {
        g_api->display.obj_set_prop(g_lbl_status, "text", "Capture failed");
    }
}

static void toggle_video_recording(void) {
    if (g_api->camera.is_recording()) {
        g_api->camera.stop_video();
        g_api->display.obj_set_prop(g_btn_shutter, "text", "● Photo");
        g_api->display.obj_set_prop(g_lbl_status, "text", "Video stopped");
    } else {
        time_t now;
        time(&now);
        struct tm tm;
        localtime_r(&now, &tm);

        char path[128];
        snprintf(path, sizeof(path), "/sdcard/media/%04d%02d%02d_%02d%02d%02d.mjpeg",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);

        g_api->camera.start_video(path, 15);
        g_api->display.obj_set_prop(g_btn_shutter, "text", "■ Stop");
        g_api->display.obj_set_prop(g_lbl_status, "text", "Recording...");
    }
}

/* ========================================================================== */
/* GALLERY SCREEN                                                             */
/* ========================================================================== */
static void show_gallery(void) {
    g_current_screen = SCREEN_GALLERY;
    g_api->display.cleanup();

    k_lvgl_obj_t header = g_api->display.obj_create("label", NULL);
    g_api->display.obj_set_prop(header, "text", "Gallery");
    g_api->display.obj_set_prop(header, "align", "top_mid");
    g_api->display.obj_set_prop(header, "y", "10");
    g_api->display.obj_set_prop(header, "text_font", "montserrat_24");

    int count = g_api->gallery.get_count();
    if (count == 0) {
        k_lvgl_obj_t empty = g_api->display.obj_create("label", NULL);
        g_api->display.obj_set_prop(empty, "text", "No photos yet");
        g_api->display.obj_set_prop(empty, "align", "center");
    } else {
        /* Grid of thumbnails */
        k_lvgl_obj_t grid = g_api->display.obj_create("obj", NULL);
        g_api->display.obj_set_prop(grid, "width", "220");
        g_api->display.obj_set_prop(grid, "height", "240");
        g_api->display.obj_set_prop(grid, "align", "top_mid");
        g_api->display.obj_set_prop(grid, "y", "50");
        g_api->display.obj_set_prop(grid, "layout", "flex_row_wrap");
        g_api->display.obj_set_prop(grid, "pad_all", "5");

        for (int i = 0; i < count && i < 12; i++) {
            media_info_t info;
            g_api->gallery.get_info(i, &info);

            k_lvgl_obj_t thumb = g_api->display.obj_create("btn", grid);
            g_api->display.obj_set_prop(thumb, "width", "70");
            g_api->display.obj_set_prop(thumb, "height", "70");

            k_lvgl_obj_t lbl = g_api->display.obj_create("label", thumb);
            char date_short[16];
            strncpy(date_short, info.date + 4, 4); // MMDD
            date_short[4] = '\0';
            g_api->display.obj_set_prop(lbl, "text", date_short);
        }
    }

    /* Back button */
    k_lvgl_obj_t btn_back = g_api->display.obj_create("btn", NULL);
    g_api->display.obj_set_prop(btn_back, "width", "100");
    g_api->display.obj_set_prop(btn_back, "height", "40");
    g_api->display.obj_set_prop(btn_back, "align", "bottom_mid");
    g_api->display.obj_set_prop(btn_back, "y", "-10");

    k_lvgl_obj_t lbl_back = g_api->display.obj_create("label", btn_back);
    g_api->display.obj_set_prop(lbl_back, "text", "Back");

    g_api->display.flush();
}

/* ========================================================================== */
/* SETTINGS SCREEN                                                            */
/* ========================================================================== */
static void show_settings(void) {
    g_current_screen = SCREEN_SETTINGS;
    g_api->display.cleanup();

    k_lvgl_obj_t header = g_api->display.obj_create("label", NULL);
    g_api->display.obj_set_prop(header, "text", "Settings");
    g_api->display.obj_set_prop(header, "align", "top_mid");
    g_api->display.obj_set_prop(header, "y", "10");
    g_api->display.obj_set_prop(header, "text_font", "montserrat_24");

    /* Resolution dropdown */
    k_lvgl_obj_t lbl_res = g_api->display.obj_create("label", NULL);
    g_api->display.obj_set_prop(lbl_res, "text", "Resolution: VGA");
    g_api->display.obj_set_prop(lbl_res, "align", "top_left");
    g_api->display.obj_set_prop(lbl_res, "x", "10");
    g_api->display.obj_set_prop(lbl_res, "y", "50");

    /* Quality slider */
    k_lvgl_obj_t lbl_qual = g_api->display.obj_create("label", NULL);
    g_api->display.obj_set_prop(lbl_qual, "text", "JPEG Quality: 12");
    g_api->display.obj_set_prop(lbl_qual, "align", "top_left");
    g_api->display.obj_set_prop(lbl_qual, "x", "10");
    g_api->display.obj_set_prop(lbl_qual, "y", "90");

    /* Telegram connect button */
    k_lvgl_obj_t btn_tg = g_api->display.obj_create("btn", NULL);
    g_api->display.obj_set_prop(btn_tg, "width", "200");
    g_api->display.obj_set_prop(btn_tg, "height", "50");
    g_api->display.obj_set_prop(btn_tg, "align", "center");

    k_lvgl_obj_t lbl_tg = g_api->display.obj_create("label", btn_tg);
    bool tg_connected = g_api->social.is_connected(0); // TELEGRAM
    g_api->display.obj_set_prop(lbl_tg, "text", tg_connected ? "Disconnect Telegram" : "Connect Telegram");

    /* Back button */
    k_lvgl_obj_t btn_back = g_api->display.obj_create("btn", NULL);
    g_api->display.obj_set_prop(btn_back, "width", "100");
    g_api->display.obj_set_prop(btn_back, "height", "40");
    g_api->display.obj_set_prop(btn_back, "align", "bottom_mid");
    g_api->display.obj_set_prop(btn_back, "y", "-10");

    k_lvgl_obj_t lbl_back = g_api->display.obj_create("label", btn_back);
    g_api->display.obj_set_prop(lbl_back, "text", "Back");

    g_api->display.flush();
}

/* ========================================================================== */
/* VOICE COMMAND HANDLER                                                      */
/* ========================================================================== */
static void on_voice_command(const char* command) {
    if (strstr(command, "take picture") || strstr(command, "take photo")) {
        capture_photo();
    } else if (strstr(command, "record video")) {
        toggle_video_recording();
    } else if (strstr(command, "open gallery")) {
        show_gallery();
    } else if (strstr(command, "post to") && strlen(g_selected_photo) > 0) {
        /* Upload selected photo */
        g_api->social.upload_photo(0, g_selected_photo, "From ESP-AppOS", NULL);
    }
}

/* ========================================================================== */
/* MAIN VIEWFINDER SCREEN                                                     */
/* ========================================================================== */
static void show_viewfinder(void) {
    g_current_screen = SCREEN_VIEWFINDER;
    g_api->display.cleanup();

    k_lvgl_obj_t header = g_api->display.obj_create("label", NULL);
    g_api->display.obj_set_prop(header, "text", "Camera");
    g_api->display.obj_set_prop(header, "align", "top_mid");
    g_api->display.obj_set_prop(header, "y", "10");
    g_api->display.obj_set_prop(header, "text_font", "montserrat_24");

    /* Preview image placeholder */
    g_img_preview = g_api->display.obj_create("img", NULL);
    g_api->display.obj_set_prop(g_img_preview, "width", "200");
    g_api->display.obj_set_prop(g_img_preview, "height", "150");
    g_api->display.obj_set_prop(g_img_preview, "align", "top_mid");
    g_api->display.obj_set_prop(g_img_preview, "y", "50");

    /* Status label */
    g_lbl_status = g_api->display.obj_create("label", NULL);
    g_api->display.obj_set_prop(g_lbl_status, "text", "Ready");
    g_api->display.obj_set_prop(g_lbl_status, "align", "center");
    g_api->display.obj_set_prop(g_lbl_status, "y", "10");

    /* Shutter button */
    g_btn_shutter = g_api->display.obj_create("btn", NULL);
    g_api->display.obj_set_prop(g_btn_shutter, "width", "80");
    g_api->display.obj_set_prop(g_btn_shutter, "height", "80");
    g_api->display.obj_set_prop(g_btn_shutter, "align", "bottom_mid");
    g_api->display.obj_set_prop(g_btn_shutter, "y", "-60");

    k_lvgl_obj_t lbl_shutter = g_api->display.obj_create("label", g_btn_shutter);
    g_api->display.obj_set_prop(lbl_shutter, "text", "● Photo");

    /* Gallery button */
    g_btn_gallery = g_api->display.obj_create("btn", NULL);
    g_api->display.obj_set_prop(g_btn_gallery, "width", "60");
    g_api->display.obj_set_prop(g_btn_gallery, "height", "60");
    g_api->display.obj_set_prop(g_btn_gallery, "align", "bottom_left");
    g_api->display.obj_set_prop(g_btn_gallery, "x", "20");
    g_api->display.obj_set_prop(g_btn_gallery, "y", "-60");

    k_lvgl_obj_t lbl_gallery = g_api->display.obj_create("label", g_btn_gallery);
    g_api->display.obj_set_prop(lbl_gallery, "text", LV_SYMBOL_IMAGE);

    /* Settings button */
    g_btn_settings = g_api->display.obj_create("btn", NULL);
    g_api->display.obj_set_prop(g_btn_settings, "width", "60");
    g_api->display.obj_set_prop(g_btn_settings, "height", "60");
    g_api->display.obj_set_prop(g_btn_settings, "align", "bottom_right");
    g_api->display.obj_set_prop(g_btn_settings, "x", "-20");
    g_api->display.obj_set_prop(g_btn_settings, "y", "-60");

    k_lvgl_obj_t lbl_settings = g_api->display.obj_create("label", g_btn_settings);
    g_api->display.obj_set_prop(lbl_settings, "text", LV_SYMBOL_SETTINGS);

    g_api->display.flush();
}

/* ========================================================================== */
/* APP ENTRY                                                                  */
/* ========================================================================== */
void app_main(const KernelAPI* api) {
    g_api = api;

    if (api->abi_version < KERNEL_ABI_VERSION) {
        api->sys.request_exit();
        return;
    }

    /* Initialize services */
    api->camera.init();
    api->gallery.scan_media();
    api->social.init();

    /* Show main screen */
    show_viewfinder();

    api->sys.log(2, "Camera", "App initialized");
}

void app_deinit(void) {
    if (g_api->camera.is_recording()) {
        g_api->camera.stop_video();
    }
}
