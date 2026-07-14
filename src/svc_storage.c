#include "svc_storage.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <sys/stat.h>

static const char* TAG = "svc_storage";
static SemaphoreHandle_t s_fs_lock; // Binary semaphore, initialized in init

/* Hardware config - adjust to your PCB */
#define SD_DETECT_GPIO      GPIO_NUM_13
#define SD_MOUNT_POINT      "/sdcard"
#define APPS_DIR            "/sdcard/apps"
#define PROVISION_FLAG_FILE "/sdcard/.provisioned"

/* Bootstrap apps downloaded on fresh card */
static const char* BOOTSTRAP_APPS[] = {
    "clock.espapp",
    "launcher.espapp",
    "settings.espapp",
    NULL
};

static struct {
    storage_state_t     state;
    sdmmc_card_t*       card;
    bool                fs_mounted;
    storage_event_cb_t  event_cb;
    SemaphoreHandle_t   state_mutex;
    TaskHandle_t        monitor_task;
    char                provision_url[256];
} s_storage;

/* ========================================================================== */
/* INTERNAL HELPERS                                                           */
/* ========================================================================== */

static void set_state(storage_state_t new_state, const char* detail) {
    xSemaphoreTake(s_storage.state_mutex, portMAX_DELAY);
    storage_state_t old = s_storage.state;
    s_storage.state = new_state;
    xSemaphoreGive(s_storage.state_mutex);

    if (old != new_state) {
        ESP_LOGW(TAG, "State: %d → %d (%s)", old, new_state, detail ? detail : "");
        if (s_storage.event_cb) {
            s_storage.event_cb(new_state, detail);
        }
    }
}

static k_err_t try_mount(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false, /* NEVER auto-format */
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4; /* 4-bit SDMMC */
    slot.cd = SD_DETECT_GPIO;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_storage.card);
    if (err == ESP_OK) {
        s_storage.fs_mounted = true;
        return K_OK;
    } else if (err == ESP_FAIL) {
        /* Card present but unformatted/corrupt FS */
        return K_ERR_IO;
    }
    return K_ERR_NOT_FOUND; /* No card */
}

static void unmount_safe(void) {
    if (s_storage.fs_mounted) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_storage.card);
        s_storage.fs_mounted = false;
        s_storage.card = NULL;
    }
}

/** Format card as FAT32 with proper cluster size for app loading */
static k_err_t format_card(void) {
    ESP_LOGW(TAG, "Formatting SD card...");
    
    /* Re-mount with format_if_mount_failed = true */
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024, /* Match app block alignment */
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.cd = SD_DETECT_GPIO;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_storage.card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Format failed: %s", esp_err_to_name(err));
        return K_ERR_IO;
    }
    s_storage.fs_mounted = true;

    /* Create directory structure */
    mkdir(APPS_DIR, 0755);
    mkdir("/sdcard/data", 0755);
    mkdir("/sdcard/cache", 0755);

    ESP_LOGI(TAG, "✓ Card formatted and directories created");
    return K_OK;
}

/** Download bootstrap apps from server */
static k_err_t provision_from_server(const char* base_url) {
    set_state(STORAGE_STATE_PROVISIONING, "Downloading apps...");

    for (int i = 0; BOOTSTRAP_APPS[i] != NULL; i++) {
        char url[384];
        char dest[128];
        snprintf(url, sizeof(url), "%s/apps/%s", base_url, BOOTSTRAP_APPS[i]);
        snprintf(dest, sizeof(dest), "%s/%s", APPS_DIR, BOOTSTRAP_APPS[i]);

        ESP_LOGI(TAG, "Downloading %s...", BOOTSTRAP_APPS[i]);

        /* Use kernel net API (implemented in Phase 1 stubs) */
        /* For now, placeholder - replace with actual http_stream implementation */
        // k_err_t res = g_kernel_api.net.http_download_file(url, dest);
        
        /* TODO: Implement streaming download with progress callback */
        /* Critical: Verify ED25519 signature BEFORE writing to final path */
        /* Write to /sdcard/cache/<name>.tmp first, verify, then rename */
        
        vTaskDelay(pdMS_TO_TICKS(100)); /* Placeholder delay */
    }

    /* Mark card as provisioned */
    FILE* f = fopen(PROVISION_FLAG_FILE, "w");
    if (f) { fprintf(f, "1"); fclose(f); }

    ESP_LOGI(TAG, "✓ Provisioning complete");
    return K_OK;
}

/* ========================================================================== */
/* MONITOR TASK                                                               */
/* Runs at low priority, polls CD pin + validates FS health                   */
/* ========================================================================== */

/*
Bug: When CD pin goes high (eject), unmount_safe() is called, but running apps may still be inside fread/fwrite calls. 
FAT filesystem state becomes corrupted.
Fix: Add a global "storage busy" lock that apps acquire during FS operations, and wait for it before unmounting.

// Apps call this via kernel API wrapper:
k_err_t svc_storage_acquire_fs(uint32_t timeout_ms) {
    if (xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return K_ERR_BUSY;
    return K_OK;
}
void svc_storage_release_fs(void) {
    xSemaphoreGive(s_fs_lock);
}

// In monitor task eject handler:
if (!cd) {
    // Wait up to 500ms for active FS ops to complete
    if (xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(500)) == pdTRUE) {
        unmount_safe();
        xSemaphoreGive(s_fs_lock);
    } else {
        ESP_LOGE(TAG, "Force unmount - FS op timed out!");
        unmount_safe(); // Acceptable risk on physical eject
    }
    set_state(STORAGE_STATE_EJECTED, "Card removed");
}
*/

static void storage_monitor_task(void* arg) {
    TickType_t last_check = xTaskGetTickCount();
    bool prev_cd = gpio_get_level(SD_DETECT_GPIO) == 0; /* Active low */

    while (1) {
        bool cd = gpio_get_level(SD_DETECT_GPIO) == 0;

        /* Edge detection: card inserted or removed */
        if (cd != prev_cd) {
            if (!cd) {
                /* Card ejected */
                unmount_safe();
                set_state(STORAGE_STATE_EJECTED, "Card removed");
            } else {
                /* Card inserted - debounce 200ms */
                vTaskDelay(pdMS_TO_TICKS(200));
                if (gpio_get_level(SD_DETECT_GPIO) == 0) {
                    k_err_t res = try_mount();
                    if (res == K_OK) {
                        /* Check if provisioned */
                        struct stat st;
                        if (stat(PROVISION_FLAG_FILE, &st) == 0) {
                            set_state(STORAGE_STATE_READY, "Card mounted");
                        } else {
                            set_state(STORAGE_STATE_UNFORMATTED, "Fresh/unprovisioned card");
                        }
                    } else if (res == K_ERR_IO) {
                        set_state(STORAGE_STATE_UNFORMATTED, "Unformatted card");
                    } else {
                        set_state(STORAGE_STATE_ERROR, "Mount failed");
                    }
                }
            }
            prev_cd = cd;
        }

        /* Periodic health check when mounted (every 5s) */
        if (s_storage.fs_mounted && (xTaskGetTickCount() - last_check > pdMS_TO_TICKS(5000))) {
            FILE* f = fopen(APPS_DIR "/.healthcheck", "r");
            if (!f) {
                /* Try creating test file to verify write access */
                f = fopen(APPS_DIR "/.healthcheck", "w");
                if (f) { fclose(f); }
                else {
                    ESP_LOGE(TAG, "SD card write failure detected!");
                    unmount_safe();
                    set_state(STORAGE_STATE_ERROR, "I/O error during operation");
                }
            } else {
                fclose(f);
            }
            last_check = xTaskGetTickCount();
        }

        vTaskDelay(pdMS_TO_TICKS(100)); /* 100ms poll rate for responsive hot-swap */
    }
}

/* ========================================================================== */
/* PUBLIC API                                                                 */
/* ========================================================================== */

k_err_t svc_storage_init(void) {
    memset(&s_storage, 0, sizeof(s_storage));
    s_storage.state_mutex = xSemaphoreCreateMutex();
    s_storage.state = STORAGE_STATE_EJECTED;

    /* Configure CD pin */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SD_DETECT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* Initial mount attempt */
    bool cd = gpio_get_level(SD_DETECT_GPIO) == 0;
    if (cd) {
        vTaskDelay(pdMS_TO_TICKS(300)); /* Power-up settle time */
        k_err_t res = try_mount();
        if (res == K_OK) {
            struct stat st;
            s_storage.state = (stat(PROVISION_FLAG_FILE, &st) == 0) 
                              ? STORAGE_STATE_READY 
                              : STORAGE_STATE_UNFORMATTED;
        } else if (res == K_ERR_IO) {
            s_storage.state = STORAGE_STATE_UNFORMATTED;
        }
    }

    /* Start monitor task */
    xTaskCreatePinnedToCore(storage_monitor_task, "stor_mon", 4096, NULL, 
                            tskIDLE_PRIORITY + 2, &s_storage.monitor_task, 0);

    ESP_LOGI(TAG, "✓ Storage manager initialized (state=%d)", s_storage.state);
    return K_OK;
}

void svc_storage_on_event(storage_event_cb_t cb) {
    s_storage.event_cb = cb;
}

storage_state_t svc_storage_get_state(void) {
    xSemaphoreTake(s_storage.state_mutex, portMAX_DELAY);
    storage_state_t s = s_storage.state;
    xSemaphoreGive(s_storage.state_mutex);
    return s;
}

k_err_t svc_storage_start_provisioning(const char* server_url) {
    storage_state_t st = svc_storage_get_state();
    if (st != STORAGE_STATE_UNFORMATTED && st != STORAGE_STATE_EJECTED) {
        return K_ERR_INVALID;
    }

    strncpy(s_storage.provision_url, server_url, sizeof(s_storage.provision_url) - 1);

    /* If unformatted, format first */
    if (st == STORAGE_STATE_UNFORMATTED) {
        k_err_t res = format_card();
        if (res != K_OK) return res;
    }

    /* Provision in background (don't block caller) */
    xTaskCreatePinnedToCore(
        [](void* arg) {
            provision_from_server(s_storage.provision_url);
            set_state(STORAGE_STATE_READY, "Provisioning complete");
            vTaskDelete(NULL);
        },
        "provision", 8192, NULL, tskIDLE_PRIORITY + 3, NULL, 0
    );

    return K_OK;
}

bool svc_storage_app_exists(const char* app_name) {
    if (s_storage.state != STORAGE_STATE_READY) return false;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.espapp", APPS_DIR, app_name);
    struct stat st;
    return stat(path, &st) == 0;
}
