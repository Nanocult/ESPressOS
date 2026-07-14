/**
 * @file app_clock.c
 * @brief ESP-AppOS Core App: Digital Clock + Calendar
 * Compile target: .espapp (Position Independent, No Stdlib)
 */
#include "kernel_api.h"

static const KernelAPI* g_api;
static k_lvgl_obj_t g_lbl_time;
static k_lvgl_obj_t g_lbl_date;
static k_lvgl_obj_t g_cal;
static k_timer_t g_timer;

/* ========================================================================== */
/* HELPERS (No libc dependency)                                               */
/* ========================================================================== */
static void fmt2(uint8_t val, char* out) {
    out[0] = '0' + (val / 10);
    out[1] = '0' + (val % 10);
    out[2] = '\0';
}

static void str_copy(char* dst, const char* src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static void str_cat(char* dst, const char* src) {
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static const char* get_day_name(uint8_t wday) {
    static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return (wday < 7) ? days[wday] : "???";
}

/* ========================================================================== */
/* TIMER CALLBACK (Runs every 1000ms)                                         */
/* ========================================================================== */
static void update_time_cb(void* ud) {
    k_datetime_t dt;
    if (g_api->clock.get_datetime(&dt) != K_OK) return;

    /* Format HH:MM:SS */
    char time_str[9];
    fmt2(dt.hour, time_str);
    time_str[2] = ':';
    fmt2(dt.min, time_str + 3);
    time_str[5] = ':';
    fmt2(dt.sec, time_str + 6);
    time_str[8] = '\0';

    g_api->display.obj_set_prop(g_lbl_time, "text", time_str);

    /* Update Date/Day label only when minute changes to save CPU */
    if (dt.sec == 0) {
        char date_str[32];
        str_copy(date_str, get_day_name(dt.wday));
        str_cat(date_str, " ");
        
        // Simple month/day formatting
        char buf[8];
        g_api->sys.itoa(dt.month, buf, 2);
        str_cat(date_str, buf);
        str_cat(date_str, "/");
        g_api->sys.itoa(dt.day, buf, 2);
        str_cat(date_str, buf);
        
        g_api->display.obj_set_prop(g_lbl_date, "text", date_str);
        
        /* Update calendar highlighted day */
        char cal_date[16];
        g_api->sys.itoa(dt.year, cal_date, 4);
        str_cat(cal_date, "-");
        g_api->sys.itoa(dt.month, buf, 2);
        str_cat(cal_date, buf);
        str_cat(cal_date, "-");
        g_api->sys.itoa(dt.day, buf, 2);
        str_cat(cal_date, buf);
        
        g_api->display.obj_set_prop(g_cal, "today", cal_date);
    }
}

/* ========================================================================== */
/* APP ENTRY & EXIT                                                           */
/* ========================================================================== */
void app_main(const KernelAPI* api) {
    g_api = api;

    if (api->abi_version < KERNEL_ABI_VERSION) {
        api->sys.log(0, "Clock", "Fatal: ABI mismatch");
        api->sys.request_exit();
        return;
    }

    api->display.init_screen(240, 320);

    /* 1. Time Label (Large) */
    g_lbl_time = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(g_lbl_time, "align", "top_mid");
    api->display.obj_set_prop(g_lbl_time, "y", "40");
    api->display.obj_set_prop(g_lbl_time, "text_font", "montserrat_48");
    api->display.obj_set_prop(g_lbl_time, "text_color", "0xFFFFFF");
    api->display.obj_set_prop(g_lbl_time, "text", "00:00:00");

    /* 2. Date/Day Label */
    g_lbl_date = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(g_lbl_date, "align", "top_mid");
    api->display.obj_set_prop(g_lbl_date, "y", "100");
    api->display.obj_set_prop(g_lbl_date, "text_font", "montserrat_24");
    api->display.obj_set_prop(g_lbl_date, "text_color", "0xAAAAAA");
    api->display.obj_set_prop(g_lbl_date, "text", "Loading...");

    /* 3. Calendar Widget */
    g_cal = api->display.obj_create("calendar", NULL);
    api->display.obj_set_prop(g_cal, "align", "bottom_mid");
    api->display.obj_set_prop(g_cal, "y", "-20");
    api->display.obj_set_prop(g_cal, "width", "220");
    api->display.obj_set_prop(g_cal, "height", "220");

    /* 4. Start 1Hz Timer */
    api->sys.timer_create(1000, true, update_time_cb, NULL, &g_timer);
    
    /* Force immediate first update */
    update_time_cb(NULL);

    api->display.flush();
    api->sys.log(2, "Clock", "App initialized successfully");
}

void app_deinit(void) {
    if (g_api) {
        g_api->sys.timer_delete(g_timer);
        g_api->sys.log(2, "Clock", "Deinitialized");
    }
}
