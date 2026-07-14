/**
 * @file app_launcher.c
 * @brief ESP-AppOS Core App: Application Launcher Grid
 */
#include "kernel_api.h"

static const KernelAPI* g_api;

/* ========================================================================== */
/* NOSTDLIB STRING UTILS                                                      */
/* ========================================================================== */
static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static int my_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char*)a - *(unsigned char*)b;
}
static void my_strcpy(char* dst, const char* src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}
static void my_strcat(char* dst, const char* src) {
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}
static const char* my_strstr(const char* haystack, const char* needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return haystack;
    }
    return 0;
}

/* ========================================================================== */
/* MINIMAL JSON PARSER                                                        */
/* Extracts string value for a given key from a flat JSON buffer.             */
/* ========================================================================== */
static bool json_get_string(const char* json, const char* key, char* out, size_t max_len) {
    char search_key[32];
    my_strcpy(search_key, "\"");
    my_strcat(search_key, key);
    my_strcat(search_key, "\"");
    
    const char* pos = my_strstr(json, search_key);
    if (!pos) return false;
    
    pos += my_strlen(search_key);
    while (*pos && *pos != ':') pos++; // Find colon
    if (!*pos) return false;
    pos++;
    while (*pos && *pos != '"') pos++; // Find opening quote
    if (!*pos) return false;
    pos++;
    
    size_t i = 0;
    while (*pos && *pos != '"' && i < max_len - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return i > 0;
}

/* ========================================================================== */
/* APP ITEM CREATION                                                          */
/* ========================================================================== */
static void on_app_clicked(k_lvgl_obj_t obj, uint32_t evt, void* user_data) {
    if (evt == 1) { // LV_EVENT_CLICKED
        const char* app_name = (const char*)user_data;
        g_api->sys.log(2, "Launcher", "Launching: %s", app_name);
        g_api->sys.request_launch(app_name);
        g_api->sys.request_exit();
    }
}

static void create_app_item(k_lvgl_obj_t parent, const char* app_name, const char* display_name) {
    /* Container for single app */
    k_lvgl_obj_t item = g_api->display.obj_create("btn", parent);
    g_api->display.obj_set_prop(item, "width", "80");
    g_api->display.obj_set_prop(item, "height", "90");
    g_api->display.obj_set_prop(item, "layout", "flex_col");
    g_api->display.obj_set_prop(item, "pad_all", "5");
    
    /* Icon (Using LVGL built-in symbols for MVP) */
    k_lvgl_obj_t icon = g_api->display.obj_create("label", item);
    g_api->display.obj_set_prop(icon, "text_font", "montserrat_24");
    
    if (my_strcmp(app_name, "clock") == 0) g_api->display.obj_set_prop(icon, "symbol", LV_SYMBOL_CLOCK);
    else if (my_strcmp(app_name, "audio") == 0) g_api->display.obj_set_prop(icon, "symbol", LV_SYMBOL_AUDIO);
    else if (my_strcmp(app_name, "settings") == 0) g_api->display.obj_set_prop(icon, "symbol", LV_SYMBOL_SETTINGS);
    else g_api->display.obj_set_prop(icon, "symbol", LV_SYMBOL_DIRECTORY);

    /* Name Label */
    k_lvgl_obj_t lbl = g_api->display.obj_create("label", item);
    g_api->display.obj_set_prop(lbl, "text", display_name);
    g_api->display.obj_set_prop(lbl, "text_font", "montserrat_14");
    
    /* Click handler - pass app_name as user_data. 
       NOTE: app_name must persist in memory. We use static array in scan_apps() */
    g_api->display.obj_on_event(item, 1, on_app_clicked, (void*)app_name);
}

/* ========================================================================== */
/* DIRECTORY SCANNER                                                          */
/* ========================================================================== */
#define MAX_APPS 16
static char g_app_names[MAX_APPS][32];
static int g_app_count = 0;

static void scan_and_render(k_lvgl_obj_t grid_parent) {
    k_dir_t dir;
    if (g_api->fs.opendir("/apps", &dir) != K_OK) {
        g_api->sys.log(0, "Launcher", "Failed to open /apps directory");
        return;
    }
    
    char filename[64];
    bool is_dir;
    char manifest_buf[512];
    
    while (g_api->fs.readdir(dir, filename, sizeof(filename), &is_dir) == K_OK) {
        /* Look for .manifest.json files */
        int len = my_strlen(filename);
        if (len > 14 && my_strcmp(filename + len - 14, ".manifest.json") == 0) {
            
            /* Extract base app name (e.g., "clock" from "clock.manifest.json") */
            char base_name[32];
            int i = 0;
            for (; i < len - 14 && i < 31; i++) base_name[i] = filename[i];
            base_name[i] = '\0';
            
            /* Read manifest */
            char path[64];
            my_strcpy(path, "/apps/");
            my_strcat(path, filename);
            
            k_file_t f;
            if (g_api->fs.open(path, K_FILE_READ, &f) == K_OK) {
                size_t read_bytes = 0;
                g_api->fs.read(f, manifest_buf, sizeof(manifest_buf) - 1, &read_bytes);
                manifest_buf[read_bytes] = '\0';
                g_api->fs.close(f);
                
                /* Parse display name */
                char display_name[32];
                if (!json_get_string(manifest_buf, "name", display_name, sizeof(display_name))) {
                    my_strcpy(display_name, base_name); // Fallback
                }
                
                /* Store name persistently for callback user_data */
                if (g_app_count < MAX_APPS) {
                    my_strcpy(g_app_names[g_app_count], base_name);
                    create_app_item(grid_parent, g_app_names[g_app_count], display_name);
                    g_app_count++;
                }
            }
        }
    }
    g_api->fs.closedir(dir);
}

/* ========================================================================== */
/* APP ENTRY                                                                  */
/* ========================================================================== */
void app_main(const KernelAPI* api) {
    g_api = api;
    
    if (api->abi_version < KERNEL_ABI_VERSION) {
        api->sys.log(0, "Launcher", "Fatal: ABI mismatch");
        api->sys.request_exit();
        return;
    }
    
    api->display.init_screen(240, 320);
    
    /* Header */
    k_lvgl_obj_t header = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(header, "text", "ESP-AppOS");
    api->display.obj_set_prop(header, "align", "top_mid");
    api->display.obj_set_prop(header, "y", "10");
    api->display.obj_set_prop(header, "text_font", "montserrat_24");
    
    /* Grid Container */
    k_lvgl_obj_t grid = api->display.obj_create("obj", NULL);
    api->display.obj_set_prop(grid, "width", "240");
    api->display.obj_set_prop(grid, "height", "260");
    api->display.obj_set_prop(grid, "align", "bottom_mid");
    api->display.obj_set_prop(grid, "y", "-10");
    api->display.obj_set_prop(grid, "layout", "flex_row_wrap");
    api->display.obj_set_prop(grid, "pad_all", "10");
    
    /* Populate grid */
    scan_and_render(grid);
    
    if (g_app_count == 0) {
        k_lvgl_obj_t err_lbl = api->display.obj_create("label", NULL);
        api->display.obj_set_prop(err_lbl, "text", "No apps found.\nCheck SD card.");
        api->display.obj_set_prop(err_lbl, "align", "center");
    }
    
    api->display.flush();
    api->sys.log(2, "Launcher", "Ready. %d apps loaded.", g_app_count);
}

void app_deinit(void) {
    g_app_count = 0; // Reset static state for next launch
}
