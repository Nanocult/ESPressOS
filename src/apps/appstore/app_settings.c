/**
 * @file app_settings.c
 * @brief ESP-AppOS Core App: Settings & OTA Updater
 */
#include "kernel_api.h"

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
static const char* my_strstr(const char* h, const char* n) {
    if (!*n) return h;
    for (; *h; h++) {
        const char* a = h, *b = n;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return h;
    }
    return 0;
}
static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* ========================================================================== */
/* APP STATE & CONSTANTS                                                      */
/* ========================================================================== */
#define MAX_APPS        16
#define MANIFEST_BUF_SZ 4096
#define URL_BUF_SZ      128
#define OTA_SERVER_URL  "https://ota.yourserver.com/manifest.json"

typedef struct {
    char name[32];
    int  build;
    char url[URL_BUF_SZ];
    bool needs_update;
} app_info_t;

static app_info_t g_local_apps[MAX_APPS];
static int g_local_count = 0;

static app_info_t g_remote_apps[MAX_APPS];
static int g_remote_count = 0;

static char g_buf[MANIFEST_BUF_SZ];

/* UI Elements */
static k_lvgl_obj_t g_lbl_status;
static k_lvgl_obj_t g_btn_check;
static k_lvgl_obj_t g_btn_home;

/* ========================================================================== */
/* JSON PARSING HELPERS                                                       */
/* ========================================================================== */
static bool json_extract_str(const char* json, const char* key, char* out, size_t max_len) {
    char search[32];
    my_strcpy(search, "\""); my_strcat(search, key); my_strcat(search, "\"");
    const char* pos = my_strstr(json, search);
    if (!pos) return false;
    pos += my_strlen(search);
    pos = skip_ws(pos);
    if (*pos != ':') return false;
    pos = skip_ws(pos + 1);
    if (*pos != '"') return false;
    pos++;
    
    size_t i = 0;
    while (*pos && *pos != '"' && i < max_len - 1) out[i++] = *pos++;
    out[i] = '\0';
    return i > 0;
}

static bool json_extract_int(const char* json, const char* key, int* out) {
    char search[32];
    my_strcpy(search, "\""); my_strcat(search, key); my_strcat(search, "\"");
    const char* pos = my_strstr(json, search);
    if (!pos) return false;
    pos += my_strlen(search);
    pos = skip_ws(pos);
    if (*pos != ':') return false;
    pos = skip_ws(pos + 1);
    
    int val = 0;
    bool found = false;
    while (*pos >= '0' && *pos <= '9') {
        val = val * 10 + (*pos - '0');
        pos++;
        found = true;
    }
    if (found) *out = val;
    return found;
}

/* ========================================================================== */
/* LOCAL APP SCANNER                                                          */
/* ========================================================================== */
static void scan_local_apps(void) {
    k_dir_t dir;
    if (g_api->fs.opendir("/apps", &dir) != K_OK) return;
    
    char fname[64];
    bool is_dir;
    while (g_api->fs.readdir(dir, fname, sizeof(fname), &is_dir) == K_OK && g_local_count < MAX_APPS) {
        int len = my_strlen(fname);
        if (len > 14 && my_strcmp(fname + len - 14, ".manifest.json") == 0) {
            char base[32];
            int i = 0;
            for (; i < len - 14 && i < 31; i++) base[i] = fname[i];
            base[i] = '\0';
            
            char path[64];
            my_strcpy(path, "/apps/"); my_strcat(path, fname);
            
            k_file_t f;
            if (g_api->fs.open(path, K_FILE_READ, &f) == K_OK) {
                size_t r = 0;
                g_api->fs.read(f, g_buf, MANIFEST_BUF_SZ - 1, &r);
                g_api->fs.close(f);
                g_buf[r] = '\0';
                
                int build = -1;
                json_extract_int(g_buf, "build", &build);
                
                my_strcpy(g_local_apps[g_local_count].name, base);
                g_local_apps[g_local_count].build = build;
                g_local_count++;
            }
        }
    }
    g_api->fs.closedir(dir);
}

/* ========================================================================== */
/* OTA DOWNLOAD PIPELINE                                                      */
/* ========================================================================== */
typedef struct {
    k_file_t file;
    k_err_t status;
} dl_ctx_t;

static k_err_t on_dl_chunk(void* ud, const void* data, size_t len) {
    dl_ctx_t* ctx = (dl_ctx_t*)ud;
    size_t written = 0;
    k_err_t res = g_api->fs.write(ctx->file, data, len, &written);
    if (res != K_OK || written != len) {
        ctx->status = K_ERR_IO;
        return K_ERR_IO; /* Abort stream */
    }
    return K_OK;
}

static void run_ota_update(void) {
    g_api->display.obj_set_prop(g_btn_check, "disabled", "1");
    g_api->display.obj_set_prop(g_lbl_status, "text", "Contacting server...");
    g_api->display.flush();
    
    /* 1. Fetch Remote Manifest */
    size_t r = 0;
    if (g_api->net.http_get(OTA_SERVER_URL, g_buf, MANIFEST_BUF_SZ, &r, 5000) != K_OK) {
        g_api->display.obj_set_prop(g_lbl_status, "text", "Network Error!");
        goto done;
    }
    g_buf[r] = '\0';
    
    /* 2. Parse Remote Apps */
    g_remote_count = 0;
    const char* pos = g_buf;
    while (g_remote_count < MAX_APPS) {
        const char* next = my_strstr(pos, "\"name\"");
        if (!next) break;
        
        app_info_t* info = &g_remote_apps[g_remote_count];
        info->needs_update = false;
        
        if (!json_extract_str(next, "name", info->name, sizeof(info->name))) break;
        if (!json_extract_int(next, "build", &info->build)) break;
        if (!json_extract_str(next, "url", info->url, sizeof(info->url))) break;
        
        pos = next + 10; /* Advance for next iteration */
        g_remote_count++;
    }
    
    /* 3. Compare Versions */
    int updates = 0;
    for (int i = 0; i < g_remote_count; i++) {
        int local_build = -1;
        for (int j = 0; j < g_local_count; j++) {
            if (my_strcmp(g_local_apps[j].name, g_remote_apps[i].name) == 0) {
                local_build = g_local_apps[j].build;
                break;
            }
        }
        if (g_remote_apps[i].build > local_build) {
            g_remote_apps[i].needs_update = true;
            updates++;
        }
    }
    
    if (updates == 0) {
        g_api->display.obj_set_prop(g_lbl_status, "text", "All apps up to date!");
        goto done;
    }
    
    /* 4. Download & Verify Loop */
    for (int i = 0; i < g_remote_count; i++) {
        if (!g_remote_apps[i].needs_update) continue;
        
        char status[64];
        my_strcpy(status, "Downloading "); my_strcat(status, g_remote_apps[i].name);
        g_api->display.obj_set_prop(g_lbl_status, "text", status);
        g_api->display.flush();
        
        char tmp_path[64];
        my_strcpy(tmp_path, "/cache/"); my_strcat(tmp_path, g_remote_apps[i].name); 
        my_strcat(tmp_path, ".espapp.tmp");
        
        k_file_t f;
        if (g_api->fs.open(tmp_path, K_FILE_WRITE | K_FILE_CREATE, &f) != K_OK) continue;
        
        dl_ctx_t ctx = { .file = f, .status = K_OK };
        k_err_t res = g_api->net.http_stream(g_remote_apps[i].url, on_dl_chunk, &ctx, 15000);
        g_api->fs.close(f);
        
        if (res != K_OK || ctx.status != K_OK) {
            g_api->fs.remove(tmp_path);
            continue;
        }
        
        /* 5. Cryptographic Verification */
        if (!g_api->sys.verify_app_file(tmp_path)) {
            g_api->display.obj_set_prop(g_lbl_status, "text", "Signature Invalid!");
            g_api->fs.remove(tmp_path);
            continue; /* Do NOT install unverified code */
        }
        
        /* 6. Atomic Rename */
        char final_path[64];
        my_strcpy(final_path, "/apps/"); my_strcat(final_path, g_remote_apps[i].name); 
        my_strcat(final_path, ".espapp");
        
        g_api->fs.remove(final_path);
        g_api->fs.rename(tmp_path, final_path);
    }
    
    g_api->display.obj_set_prop(g_lbl_status, "text", "Updates Installed!");

done:
    g_api->display.obj_set_prop(g_btn_check, "disabled", "0");
}

/* ========================================================================== */
/* UI EVENT HANDLERS                                                          */
/* ========================================================================== */
static void on_check_updates(k_lvgl_obj_t obj, uint32_t evt, void* ud) {
    if (evt == 1) run_ota_update();
}

static void on_go_home(k_lvgl_obj_t obj, uint32_t evt, void* ud) {
    if (evt == 1) {
        g_api->sys.request_launch("launcher");
        g_api->sys.request_exit();
    }
}

/* ========================================================================== */
/* APP ENTRY                                                                  */
/* ========================================================================== */
void app_main(const KernelAPI* api) {
    g_api = api;
    api->display.init_screen(240, 320);
    
    k_lvgl_obj_t header = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(header, "text", "Settings & OTA");
    api->display.obj_set_prop(header, "align", "top_mid");
    api->display.obj_set_prop(header, "y", "20");
    api->display.obj_set_prop(header, "text_font", "montserrat_24");
    
    g_btn_check = api->display.obj_create("btn", NULL);
    api->display.obj_set_prop(g_btn_check, "width", "200");
    api->display.obj_set_prop(g_btn_check, "height", "50");
    api->display.obj_set_prop(g_btn_check, "align", "top_mid");
    api->display.obj_set_prop(g_btn_check, "y", "80");
    
    k_lvgl_obj_t check_lbl = api->display.obj_create("label", g_btn_check);
    api->display.obj_set_prop(check_lbl, "text", "Check for Updates");
    api->display.obj_on_event(g_btn_check, 1, on_check_updates, NULL);
    
    g_lbl_status = api->display.obj_create("label", NULL);
    api->display.obj_set_prop(g_lbl_status, "text", "Ready.");
    api->display.obj_set_prop(g_lbl_status, "align", "center");
    api->display.obj_set_prop(g_lbl_status, "text_font", "montserrat_16");
    
    g_btn_home = api->display.obj_create("btn", NULL);
    api->display.obj_set_prop(g_btn_home, "width", "200");
    api->display.obj_set_prop(g_btn_home, "height", "50");
    api->display.obj_set_prop(g_btn_home, "align", "bottom_mid");
    api->display.obj_set_prop(g_btn_home, "y", "-30");
    
    k_lvgl_obj_t home_lbl = api->display.obj_create("label", g_btn_home);
    api->display.obj_set_prop(home_lbl, "text", "Return to Home");
    api->display.obj_on_event(g_btn_home, 1, on_go_home, NULL);
    
    scan_local_apps(); /* Pre-cache local versions */
    api->display.flush();
}

void app_deinit(void) {
    /* No background threads or persistent handles to clean up */
}
