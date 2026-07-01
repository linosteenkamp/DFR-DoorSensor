/**
 * @file main.c
 * @brief DFR-DoorSensor entry point. ESP32-C6 Zigbee-router door sensor.
 *        Wiring is filled in across the implementation plan tasks.
 */
#include "esp_log.h"
#include "door_contact.h"

#define DOOR_CONTACT_GPIO        5
#define DOOR_DEBOUNCE_MS         40
#define DOOR_CONTACT_ACTIVE_LOW  1

static const char *TAG = "MAIN";

static void on_door_change(bool open)
{
    ESP_LOGI(TAG, "callback: door is now %s", open ? "OPEN" : "CLOSED");
}

void app_main(void)
{
    ESP_LOGI(TAG, "DFR-DoorSensor boot (door_contact demo)");
    door_contact_init(DOOR_CONTACT_GPIO, DOOR_DEBOUNCE_MS,
                      DOOR_CONTACT_ACTIVE_LOW, on_door_change);
}
