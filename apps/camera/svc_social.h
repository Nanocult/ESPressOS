#ifndef SVC_SOCIAL_H
#define SVC_SOCIAL_H

#include "kernel_api.h"

typedef enum {
    SOCIAL_PLATFORM_TELEGRAM,
    SOCIAL_PLATFORM_X,
    SOCIAL_PLATFORM_INSTAGRAM,
} social_platform_t;

k_err_t svc_social_init(void);
bool svc_social_is_connected(social_platform_t platform);
k_err_t svc_social_connect_telegram(const char* bot_token, const char* chat_id);
k_err_t svc_social_disconnect(social_platform_t platform);
k_err_t svc_social_upload_photo(social_platform_t platform, const char* path, const char* caption, void (*progress_cb)(int percent));

#endif
