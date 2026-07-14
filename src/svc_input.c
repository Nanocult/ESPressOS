#include "svc_input.h"
#include "svc_clock.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char* TAG = "svc_input";

/* Hardware mapping - adjust to your PCB */
#define BTN_BACK_GPIO     GPIO_NUM_0
#define BTN_UP_GPIO       GPIO_NUM_1
#define BTN_DOWN_GPIO     GPIO_NUM_2
#define BTN_POWER_GPIO    GPIO_NUM_3
#define TOUCH_SCL         GPIO_NUM_8
#define TOUCH_SDA         GPIO_NUM_9
#define ENCODER_A_GPIO    GPIO_NUM_10
#define ENCODER_B_GPIO    GPIO_NUM_11

#define LONG_PRESS_MS     800
#define FORCE_QUIT_MS     2000
#define DOUBLE_TAP_MS     300
#define DEBOUNCE_MS       20
#define MAX_SUBSCRIBERS   16
#define EVT_QUEUE_LEN     32

/* Subscriber registry */
typedef struct {
    uint32_t        mask;
    input_callback_t cb;
    void*           user_data;
    bool            active;
} subscriber_t;

static struct {
    subscriber_t    subs[MAX_SUBSCRIBERS];
    QueueHandle_t   evt_queue;
    SemaphoreHandle_t sub_mutex;
    sys_gesture_t   pending_gesture;
    TaskHandle_t    poll_task;
    
    /* Button state tracking */
    struct {
        gpio_num_t gpio;
        bool       pressed;
        uint32_t   press_time;
        bool       long_fired;
    } buttons[4];
} s_input;

/* ========================================================================== */
/* BUTTON POLLING WITH DEBOUNCE + LONG PRESS                                  */
/* ========================================================================== */
static void poll_buttons(void) {
    uint32_t now = svc_clock_tick_ms();
    
    for (int i = 0; i < 4; i++) {
        bool level = gpio_get_level(s_input.buttons[i].gpio) == 0; // Active low
        
        if (level && !s_input.buttons[i].pressed) {
            // Rising edge (press)
            s_input.buttons[i].pressed = true;
            s_input.buttons[i].press_time = now;
            s_input.buttons[i].long_fired = false;
            
            input_event_t evt = {
                .type = INPUT_EVT_BTN_PRESS,
                .button = { .btn_id = i, .hold_ms = 0 },
                .timestamp_ms = now
            };
            xQueueSend(s_input.evt_queue, &evt, 0);
            
        } else if (!level && s_input.buttons[i].pressed) {
            // Falling edge (release)
            s_input.buttons[i].pressed = false;
            uint32_t held = now - s_input.buttons[i].press_time;
            
            input_event_t evt = {
                .type = INPUT_EVT_BTN_RELEASE,
                .button = { .btn_id = i, .hold_ms = held },
                .timestamp_ms = now
            };
            xQueueSend(s_input.evt_queue, &evt, 0);
            
        } else if (level && s_input.buttons[i].pressed && !s_input.buttons[i].long_fired) {
            // Check for long press
            uint32_t held = now - s_input.buttons[i].press_time;
            if (held >= LONG_PRESS_MS) {
                s_input.buttons[i].long_fired = true;
                
                input_event_t evt = {
                    .type = INPUT_EVT_BTN_LONG,
                    .button = { .btn_id = i, .hold_ms = held },
                    .timestamp_ms = now
                };
                xQueueSend(s_input.evt_queue, &evt, 0);
                
                // Detect system gestures
                if (i == 1) s_input.pending_gesture = SYS_GESTURE_VOL_UP;
                else if (i == 2) s_input.pending_gesture = SYS_GESTURE_VOL_DOWN;
                else if (i == 0 && held >= FORCE_QUIT_MS) {
                    s_input.pending_gesture = SYS_GESTURE_FORCE_QUIT;
                }
            }
        }
    }
}

/* ========================================================================== */
/* EVENT DISPATCH TASK                                                        */
/* ========================================================================== */
static void input_dispatch_task(void* arg) {
    input_event_t evt;
    
    while (1) {
        if (xQueueReceive(s_input.evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
            /* System gestures consume specific events */
            bool consumed = false;
            if (evt.type == INPUT_EVT_BTN_LONG && s_input.pending_gesture != SYS_GESTURE_NONE) {
                consumed = true; // Don't forward to apps
                ESP_LOGI(TAG, "System gesture: %d", s_input.pending_gesture);
            }
            
            if (!consumed) {
                xSemaphoreTake(s_input.sub_mutex, portMAX_DELAY);
                for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
                    if (s_input.subs[i].active && 
                        (s_input.subs[i].mask & evt.type)) {
                        s_input.subs[i].cb(&evt, s_input.subs[i].user_data);
                    }
                }
                xSemaphoreGive(s_input.sub_mutex);
            }
        }
    }
}

/* ========================================================================== */
/* PUBLIC API                                                                 */
/* ========================================================================== */
k_err_t svc_input_init(void) {
    memset(&s_input, 0, sizeof(s_input));
    
    s_input.evt_queue = xQueueCreate(EVT_QUEUE_LEN, sizeof(input_event_t));
    s_input.sub_mutex = xSemaphoreCreateMutex();
    
    /* Configure button GPIOs */
    gpio_num_t btn_gpios[] = {BTN_BACK_GPIO, BTN_UP_GPIO, BTN_DOWN_GPIO, BTN_POWER_GPIO};
    for (int i = 0; i < 4; i++) {
        s_input.buttons[i].gpio = btn_gpios[i];
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << btn_gpios[i]),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&cfg);
    }
    
    /* TODO: Initialize touch controller (FT6236/CST816S via I2C) */
    /* TODO: Initialize rotary encoder (PCNT peripheral preferred) */
    
    /* Start dispatch task */
    xTaskCreatePinnedToCore(input_dispatch_task, "inp_disp", 3072, NULL,
                            tskIDLE_PRIORITY + 4, &s_input.poll_task, 0);
    
    /* Start polling timer (10ms resolution for responsive buttons) */
    esp_timer_create_args_t poll_cfg = {
        .callback = (esp_timer_cb_t)poll_buttons,
        .name = "btn_poll"
    };
    esp_timer_handle_t poll_timer;
    esp_timer_create(&poll_cfg, &poll_timer);
    esp_timer_start_periodic(poll_timer, 10 * 1000); // 10ms
    
    ESP_LOGI(TAG, "✓ Input manager initialized (4 buttons, touch, encoder)");
    return K_OK;
}

int svc_input_subscribe(uint32_t event_mask, input_callback_t cb, void* user_data) {
    xSemaphoreTake(s_input.sub_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (!s_input.subs[i].active) {
            s_input.subs[i].mask = event_mask;
            s_input.subs[i].cb = cb;
            s_input.subs[i].user_data = user_data;
            s_input.subs[i].active = true;
            xSemaphoreGive(s_input.sub_mutex);
            return i;
        }
    }
    xSemaphoreGive(s_input.sub_mutex);
    ESP_LOGW(TAG, "Subscriber limit reached (%d)", MAX_SUBSCRIBERS);
    return -1;
}

void svc_input_unsubscribe(int sub_id) {
    if (sub_id < 0 || sub_id >= MAX_SUBSCRIBERS) return;
    xSemaphoreTake(s_input.sub_mutex, portMAX_DELAY);
    s_input.subs[sub_id].active = false;
    xSemaphoreGive(s_input.sub_mutex);
}

sys_gesture_t svc_input_consume_sys_gesture(void) {
    sys_gesture_t g = s_input.pending_gesture;
    s_input.pending_gesture = SYS_GESTURE_NONE;
    return g;
}
