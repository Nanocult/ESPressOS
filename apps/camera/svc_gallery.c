#include "svc_gallery.h"
#include "esp_log.h"
#include "esp_jpg_decode.h"
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>

static const char* TAG = "svc_gallery";
static media_info_t s_media[256]; // Max 256 files
static int s_media_count = 0;

k_err_t svc_gallery_init(void) {
    s_media_count = 0;
    
    DIR* dir = opendir("/sdcard/media");
    if (!dir) {
        ESP_LOGW(TAG, "/media directory not found");
        return K_OK;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL && s_media_count < 256) {
        if (entry->d_type != DT_REG) continue;

        char path[128];
        snprintf(path, sizeof(path), "/sdcard/media/%s", entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        /* Check if JPEG or MJPEG */
        int len = strlen(entry->d_name);
        bool is_jpg = (len > 4 && strcasecmp(entry->d_name + len - 4, ".jpg") == 0);
        bool is_mjpeg = (len > 6 && strcasecmp(entry->d_name + len - 6, ".mjpeg") == 0);

        if (is_jpg || is_mjpeg) {
            strncpy(s_media[s_media_count].path, path, sizeof(s_media[s_media_count].path) - 1);
            s_media[s_media_count].size_kb = st.st_size / 1024;
            s_media[s_media_count].is_video = is_mjpeg;
            
            /* Extract date from filename (YYYYMMDD_HHMMSS) */
            strncpy(s_media[s_media_count].date, entry->d_name, 15);
            s_media[s_media_count].date[15] = '\0';
            
            s_media_count++;
        }
    }
    closedir(dir);

    ESP_LOGI(TAG, "✓ Gallery indexed %d media files", s_media_count);
    return K_OK;
}

int svc_gallery_get_count(void) {
    return s_media_count;
}

k_err_t svc_gallery_get_info(int index, media_info_t* out_info) {
    if (index < 0 || index >= s_media_count || !out_info) return K_ERR_INVALID;
    *out_info = s_media[index];
    return K_OK;
}

k_err_t svc_gallery_generate_thumbnail(const char* src_path, uint8_t* rgb565_buf, int width, int height) {
    /* Read JPEG file */
    FILE* f = fopen(src_path, "rb");
    if (!f) return K_ERR_IO;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* jpg_buf = malloc(size);
    if (!jpg_buf) {
        fclose(f);
        return K_ERR_NOMEM;
    }

    fread(jpg_buf, 1, size, f);
    fclose(f);

    /* Decode JPEG to RGB565 and downscale */
    /* ESP-IDF provides esp_jpg_decode() for this */
    esp_err_t err = esp_jpg_decode(size, jpg_buf, rgb565_buf, width, height);
    
    free(jpg_buf);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Thumbnail decode failed: %s", esp_err_to_name(err));
        return K_ERR_IO;
    }

    return K_OK;
}

k_err_t svc_gallery_delete(const char* path) {
    if (remove(path) != 0) return K_ERR_IO;
    
    /* Re-index gallery */
    svc_gallery_init();
    return K_OK;
}
