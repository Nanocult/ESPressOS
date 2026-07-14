#ifndef SVC_INPUT_H
#define SVC_INPUT_H

#include "kernel_api.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Input event types */
typedef enum {
    INPUT_EVT_BTN_PRESS     = 0x01,
    INPUT_EVT_BTN_RELEASE   = 0x02,
    INPUT_EVT_BTN_LONG      = 0x04,  /* Held > 800ms */
    INPUT_EVT_TOUCH_DOWN    = 0x10,
    INPUT_EVT_TOUCH_UP      = 0x20,
    INPUT_EVT_TOUCH_MOVE    = 0x40,
    INPUT_EVT_ENCODER_CW    = 0x80,
    INPUT_EVT_ENCODER_CCW   = 0x100,
} input_event_type_t;

/** Raw input event data */
typedef struct {
    input_event_type_t type;
    union {
        struct { uint8_t btn_id; uint32_t hold_ms; } button;
        struct { int16_t x; int16_t y; } touch;
        struct { int8_t delta; } encoder;
    };
    uint32_t timestamp_ms;
} input_event_t;

/** Callback signature for input subscribers */
typedef void (*input_callback_t)(const input_event_t* evt, void* user_data);

/** System gestures that consume events before apps see them */
typedef enum {
    SYS_GESTURE_NONE = 0,
    SYS_GESTURE_VOL_UP,       /* Long press BTN_UP */
    SYS_GESTURE_VOL_DOWN,     /* Long press BTN_DOWN */
    SYS_GESTURE_FORCE_QUIT,   /* Long press BTN_BACK > 2s */
    SYS_GESTURE_SCREENSHOT,   /* Double tap BTN_POWER */
} sys_gesture_t;

/** Initialize input manager. Call once at boot. */
k_err_t svc_input_init(void);

/** Subscribe to input events matching mask. Returns subscription ID. */
int svc_input_subscribe(uint32_t event_mask, input_callback_t cb, void* user_data);

/** Unsubscribe by ID */
void svc_input_unsubscribe(int sub_id);

/** Get last detected system gesture (consumed by kernel, not forwarded to apps) */
sys_gesture_t svc_input_consume_sys_gesture(void);

#ifdef __cplusplus
}
#endif
#endif
