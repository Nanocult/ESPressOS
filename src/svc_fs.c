#include <dirent.h>
#include <sys/stat.h>

k_err_t svc_fs_opendir(const char* path, k_dir_t* dir) {
    char full_path[128];
    // Allow launcher to read the global apps directory
    if (strncmp(path, "/apps", 5) == 0) {
        snprintf(full_path, sizeof(full_path), "/sdcard%s", path);
    } else {
        // Sandbox to app data dir for others
        snprintf(full_path, sizeof(full_path), "/sdcard/data/%s/%s", 
                 kernel_get_current_app_name(), path);
    }
    
    DIR* d = opendir(full_path);
    if (!d) return K_ERR_NOT_FOUND;
    *dir = (k_dir_t)d;
    return K_OK;
}

k_err_t svc_fs_readdir(k_dir_t dir, char* out_name, size_t max_len, bool* is_dir) {
    DIR* d = (DIR*)dir;
    struct dirent* entry = readdir(d);
    if (!entry) return K_ERR_EOF;
    
    strncpy(out_name, entry->d_name, max_len - 1);
    out_name[max_len - 1] = '\0';
    *is_dir = (entry->d_type == DT_DIR);
    return K_OK;
}

k_err_t svc_fs_closedir(k_dir_t dir) {
    if (dir) closedir((DIR*)dir);
    return K_OK;
}
