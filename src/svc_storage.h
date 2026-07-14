/**
 * @file svc_storage.h
 * @brief Persistent Storage Manager - SD Card Hot-Swap & Provisioning
 */
#ifndef SVC_STORAGE_H
#define SVC_STORAGE_H

#include "kernel_api.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Storage state machine states */
typedef enum {
    STORAGE_STATE_READY,          /* Card mounted, apps accessible */
    STORAGE_STATE_EJECTED,        /* Card physically removed */
    STORAGE_STATE_UNFORMATTED,    /* Card inserted but no valid FS */
    STORAGE_STATE_PROVISIONING,   /* Downloading bootstrap apps from server */
    STORAGE_STATE_ERROR,          /* Unrecoverable I/O error */
} storage_state_t;

/** Event callback for UI/Lifecycle manager to react */
typedef void (*storage_event_cb_t)(storage_state_t new_state, const char* detail);

/**
 * Initialize storage manager. Must be called before app_loader.
 * Sets up SDMMC detection pin + mount attempt.
 */
k_err_t svc_storage_init(void);

/** Register callback for state changes (called from storage task context) */
void svc_storage_on_event(storage_event_cb_t cb);

/** Get current storage state (safe to call from any task) */
storage_state_t svc_storage_get_state(void);

/**
 * Trigger re-provisioning manually (e.g., user confirms "Download Apps?" dialog).
 * Only valid in STORAGE_STATE_UNFORMATTED or STORAGE_STATE_EJECTED.
 */
k_err_t svc_storage_start_provisioning(const char* server_url);

/** Check if a specific core app exists on card */
bool svc_storage_app_exists(const char* app_name);

#ifdef __cplusplus
}
#endif
#endif
