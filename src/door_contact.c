/**
 * @file door_contact.c
 * @brief KW11-2N door switch on a GPIO: both-edge ISR restarts a one-shot
 *        esp_timer; on expiry the settled level is debounced + polarity-mapped
 *        and a state-change callback is fired. Pure logic lives in
 *        contact_map.c / debounce.c (host-tested).
 */
#include "door_contact.h"
#include "contact_map.h"
#include "debounce.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "DOOR";

static int               s_gpio;
static bool              s_active_low;
static uint32_t          s_debounce_us;
static door_contact_cb_t s_cb;
static esp_timer_handle_t s_timer;
static debounce_t        s_dbnc;
static volatile bool     s_open;

/* Timer callback (esp_timer task context): sample the settled level, debounce,
 * map polarity, fire the callback only on a real transition. */
static void settle_timer_cb(void *arg)
{
    (void)arg;
    int level = gpio_get_level((gpio_num_t)s_gpio);
    if (debounce_settle(&s_dbnc, level)) {
        s_open = contact_is_open(level, s_active_low);
        ESP_LOGI(TAG, "door %s", s_open ? "OPEN" : "CLOSED");
        if (s_cb) {
            s_cb(s_open);
        }
    }
}

/* ISR: restart the one-shot debounce timer. Keep it minimal — no logging, no
 * callback here. esp_timer_stop/start_once are ISR-safe. */
static void IRAM_ATTR gpio_isr(void *arg)
{
    (void)arg;
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, s_debounce_us);
}

esp_err_t door_contact_init(int gpio, uint32_t debounce_ms, bool active_low,
                            door_contact_cb_t cb)
{
    s_gpio        = gpio;
    s_active_low  = active_low;
    s_debounce_us = debounce_ms * 1000U;
    s_cb          = cb;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* Seed the debounced state from the current level so the first real edge is
     * reported correctly and door_contact_is_open() is valid immediately. */
    int level = gpio_get_level((gpio_num_t)gpio);
    debounce_init(&s_dbnc, level);
    s_open = contact_is_open(level, active_low);

    const esp_timer_create_args_t targs = {
        .callback = settle_timer_cb,
        .name     = "door_debounce",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_timer));

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add((gpio_num_t)gpio, gpio_isr, NULL));

    ESP_LOGI(TAG, "door_contact init: gpio=%d debounce=%ums active_low=%d start=%s",
             gpio, debounce_ms, active_low, s_open ? "OPEN" : "CLOSED");
    return ESP_OK;
}

bool door_contact_is_open(void)
{
    return s_open;
}
