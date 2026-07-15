#include "svc_social.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char* TAG = "svc_social";
static nvs_handle_t s_nvs;

typedef struct {
    char bot_token[64];
    char chat_id[32];
    bool connected;
} telegram_config_t;

static telegram_config_t s_telegram = {0};

k_err_t svc_social_init(void) {
    esp_err_t err = nvs_open("social", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) return K_ERR_IO;

    /* Load saved Telegram config */
    size_t len = sizeof(s_telegram.bot_token);
    nvs_get_str(s_nvs, "tg_token", s_telegram.bot_token, &len);
    len = sizeof(s_telegram.chat_id);
    nvs_get_str(s_nvs, "tg_chat", s_telegram.chat_id, &len);
    
    s_telegram.connected = (strlen(s_telegram.bot_token) > 0 && strlen(s_telegram.chat_id) > 0);

    ESP_LOGI(TAG, "✓ Social service initialized (Telegram: %s)", s_telegram.connected ? "connected" : "not connected");
    return K_OK;
}

bool svc_social_is_connected(social_platform_t platform) {
    if (platform == SOCIAL_PLATFORM_TELEGRAM) return s_telegram.connected;
    return false;
}

k_err_t svc_social_connect_telegram(const char* bot_token, const char* chat_id) {
    if (!bot_token || !chat_id) return K_ERR_INVALID;

    strncpy(s_telegram.bot_token, bot_token, sizeof(s_telegram.bot_token) - 1);
    strncpy(s_telegram.chat_id, chat_id, sizeof(s_telegram.chat_id) - 1);
    s_telegram.connected = true;

    /* Save to NVS */
    nvs_set_str(s_nvs, "tg_token", s_telegram.bot_token);
    nvs_set_str(s_nvs, "tg_chat", s_telegram.chat_id);
    nvs_commit(s_nvs);

    ESP_LOGI(TAG, "Telegram connected");
    return K_OK;
}

k_err_t svc_social_disconnect(social_platform_t platform) {
    if (platform == SOCIAL_PLATFORM_TELEGRAM) {
        s_telegram.connected = false;
        s_telegram.bot_token[0] = '\0';
        s_telegram.chat_id[0] = '\0';
        nvs_erase_key(s_nvs, "tg_token");
        nvs_erase_key(s_nvs, "tg_chat");
        nvs_commit(s_nvs);
    }
    return K_OK;
}

k_err_t svc_social_upload_photo(social_platform_t platform, const char* path, const char* caption, void (*progress_cb)(int percent)) {
    if (platform != SOCIAL_PLATFORM_TELEGRAM || !s_telegram.connected) return K_ERR_INVALID;

    /* Read photo file */
    FILE* f = fopen(path, "rb");
    if (!f) return K_ERR_IO;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* file_buf = malloc(file_size);
    if (!file_buf) {
        fclose(f);
        return K_ERR_NOMEM;
    }

    fread(file_buf, 1, file_size, f);
    fclose(f);

    /* Build Telegram API URL */
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", s_telegram.bot_token);

    /* Build multipart form data */
    char boundary[64] = "----ESPAppOSBoundary";
    char body[4096];
    int body_len = 0;

    /* Chat ID field */
    body_len += snprintf(body + body_len, sizeof(body) - body_len,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n"
        "%s\r\n",
        boundary, s_telegram.chat_id);

    /* Caption field (if provided) */
    if (caption && strlen(caption) > 0) {
        body_len += snprintf(body + body_len, sizeof(body) - body_len,
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"caption\"\r\n\r\n"
            "%s\r\n",
            boundary, caption);
    }

    /* Photo file field */
    body_len += snprintf(body + body_len, sizeof(body) - body_len,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"photo\"; filename=\"photo.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n",
        boundary);

    /* Allocate full request buffer */
    int total_len = body_len + file_size + strlen(boundary) + 10;
    uint8_t* req_buf = malloc(total_len);
    if (!req_buf) {
        free(file_buf);
        return K_ERR_NOMEM;
    }

    memcpy(req_buf, body, body_len);
    memcpy(req_buf + body_len, file_buf, file_size);
    int footer_len = snprintf((char*)req_buf + body_len + file_size, total_len - body_len - file_size,
        "\r\n--%s--\r\n", boundary);

    free(file_buf);

    /* Send HTTP POST */
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "multipart/form-data; boundary=----ESPAppOSBoundary");
    esp_http_client_set_post_field(client, (char*)req_buf, body_len + file_size + footer_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    free(req_buf);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "Telegram upload failed: %s (HTTP %d)", esp_err_to_name(err), status);
        return K_ERR_IO;
    }

    ESP_LOGI(TAG, "✓ Photo uploaded to Telegram");
    return K_OK;
}
