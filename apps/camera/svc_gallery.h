#ifndef SVC_GALLERY_H
#define SVC_GALLERY_H

#include "kernel_api.h"

typedef struct {
    char path[128];
    char date[32];
    int size_kb;
    bool is_video;
} media_info_t;

k_err_t svc_gallery_init(void);
int svc_gallery_get_count(void);
k_err_t svc_gallery_get_info(int index, media_info_t* out_info);
k_err_t svc_gallery_generate_thumbnail(const char* src_path, uint8_t* rgb565_buf, int width, int height);
k_err_t svc_gallery_delete(const char* path);

#endif
