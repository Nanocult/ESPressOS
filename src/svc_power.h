/**
 * @file svc_power.h
 * @brief System Power State & Lock Manager
 */
#ifndef SVC_POWER_H
#define SVC_POWER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    PM_STATE_ACTIVE,       /* Display ON, CPU Awake */
    PM_STATE_DIMMED,       /* Display Backlight 20%, CPU Awake */
    PM_STATE_DISPLAY_OFF,  /* Display OFF, CPU Awake (e.g., Audio playing) */
    PM_STATE_LIGHT_SLEEP,  /* Display OFF, CPU sleeping (waking for VAD) */
    PM_STATE_DEEP_SLEEP    /* System hibernating (requires button/RTC to wake) */
} pm_state_t;

typedef enum {
    PM_LOCK_NONE = 0,
    PM_LOCK_DISPLAY_ON,    /* Prevents display dim/off (e.g., User reading text) */
    PM_LOCK_CPU_AWAKE,     /* Prevents light sleep (e.g., Audio decoding, Network) */
    PM_LOCK_FULL_AWAKE     /* Prevents all power saving (e.g., Active Voice Recording) */
} pm_lock_t;

void svc_power_init(void);

/** Apps call this to prevent sleep. Locks stack (highest lock wins). */
void svc_power_acquire(pm_lock_t lock);
void svc_power_release(pm_lock_t lock);

/** Get current system state */
pm_state_t svc_power_get_state(void);

/** Manually wake system (e.g., from button press) */
void svc_power_wake(void);

#endif
