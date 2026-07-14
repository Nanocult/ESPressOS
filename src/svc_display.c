/**
 * @file svc_display.c
 * @brief Persistent Display Manager - LVGL host, survives app transitions
 */
#include "kernel_api.h"
#include "svc_display.h"
#include "lvgl.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char* TAG = "svc_display";

/* Permanent LVGL buffers in PSRAM */
#define DISP_BUF_SIZE   (240 * 320 / 10 * sizeof(lv_color_t)) /* ~15KB per buffer */
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t* s_buf1 = NULL;
static lv_color_t* s_buf2 = NULL;
static lv_disp_drv_t s_disp_drv;
static esp_lcd_panel_handle_t s_panel = NULL;

/* Per-app tracking for cleanup */
#define MAX_APP_OBJECTS 128
static lv_obj_t* s_app_objects[MAX_APP_OBJECTS];
static uint16_t  s_app_obj_count = 0;

/* ========================================================================== */
/* LVGL FLUSH CALLBACK                                                        */
/* ========================================================================== */
static void disp_flush_cb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map) {
    int x1 = area->x1, y1 = area->y1, x2 = area->x2 + 1, y2 = area->y2 + 1;
    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, color_map);
    lv_disp_flush_ready(drv);
}

/* ========================================================================== */
/* INITIALIZATION                                                             */
/* ========================================================================== */
void svc_display_init(void) {
    /* Initialize LCD panel (ST7789 SPI example - adapt to your hardware) */
    // ... esp_lcd initialization code omitted for brevity ...
    
    /* Allocate double buffers in PSRAM */
    s_buf1 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_buf2 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    assert(s_buf1 && s_buf2);

    /* Init LVGL */
    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, DISP_BUF_SIZE / sizeof(lv_color_t));
    
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = 240;
    s_disp_drv.ver_res = 320;
    s_disp_drv.flush_cb = disp_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    /* LVGL tick timer (1ms) */
    lv_tick_inc(1); // Replace with esp_timer periodic callback in production

    ESP_LOGI(TAG, "✓ Display manager initialized (240×320, double-buffered)");
}

/* ========================================================================== */
/* KERNEL API IMPLEMENTATIONS                                                 */
/* ========================================================================== */

k_err_t svc_display_init_screen(uint16_t w, uint16_t h) {
    /* Clear previous app's objects */
    svc_display_cleanup();
    
    /* Create fresh root screen for new app */
    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    s_app_obj_count = 0;
    
    ESP_LOGD(TAG, "Screen initialized for new app (%ux%u)", w, h);
    return K_OK;
}

k_lvgl_obj_t svc_display_obj_create(const char* type, k_lvgl_obj_t parent) {
    if (s_app_obj_count >= MAX_APP_OBJECTS) {
        ESP_LOGW(TAG, "App object limit reached (%d)", MAX_APP_OBJECTS);
        return NULL;
    }
    
    lv_obj_t* obj = NULL;
    lv_obj_t* par = parent ? (lv_obj_t*)parent : lv_scr_act();
    
    /* Simple type dispatch - extend as needed */
    if (strcmp(type, "label") == 0)      obj = lv_label_create(par);
    else if (strcmp(type, "btn") == 0)   obj = lv_btn_create(par);
    else if (strcmp(type, "img") == 0)   obj = lv_img_create(par);
    else if (strcmp(type, "slider") == 0)obj = lv_slider_create(par);
    else {
        ESP_LOGW(TAG, "Unknown widget type: %s", type);
        return NULL;
    }
    
    /* Track for cleanup */
    s_app_objects[s_app_obj_count++] = obj;
    return (k_lvgl_obj_t)obj;
}

void svc_display_cleanup(void) {
    /* Delete all objects created by current app */
    for (uint16_t i = 0; i < s_app_obj_count; i++) {
        if (s_app_objects[i]) {
            lv_obj_del(s_app_objects[i]);
            s_app_objects[i] = NULL;
        }
    }
    s_app_obj_count = 0;
    ESP_LOGD(TAG, "Display cleaned up");
}

void svc_display_flush(void) {
    lv_refr_now(NULL);
}
