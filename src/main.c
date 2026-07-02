/**
 * @file main.c
 * @brief DFR-DoorSensor: ESP32-C6 Zigbee-router door sensor.
 *
 * app_main() runs once and never sleeps: init NVS + event loop, configure the
 * door GPIO (ISR + debounce), start the Zigbee router. A door edge -> callback
 * -> zb_router_set_contact() -> IAS Zone ZoneStatus report.
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "door_contact.h"
#include "zb_router.h"
#include "ota_client.h"

#define DEFAULT_DEVICE_ID        "door01"   /* Basic model/name; rename in z2m UI */
#define DOOR_CONTACT_GPIO        5
#define DOOR_DEBOUNCE_MS         40
#define DOOR_CONTACT_ACTIVE_LOW  0   /* HIGH = open: door CLOSED shorts the pull-up to GND (pin LOW). Verified on-device against z2m v1.0.4 */
#define JOIN_TIMEOUT_MS          30000

static const char *TAG = "MAIN";

/* Door state-change callback (esp_timer task context): push to Zigbee. */
static void on_door_change(bool open)
{
    zb_router_set_contact(open);
}

static void init_system(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
}

void app_main(void)
{
    ESP_LOGI(TAG, "DFR-DoorSensor boot (%s)", DEFAULT_DEVICE_ID);
    init_system();

    /* Door GPIO first so the initial state is known before/at join. */
    door_contact_init(DOOR_CONTACT_GPIO, DOOR_DEBOUNCE_MS,
                      DOOR_CONTACT_ACTIVE_LOW, on_door_change);

    if (zb_router_init() != ESP_OK) {
        ESP_LOGE(TAG, "Zigbee init failed — restarting");
        esp_restart();
    }

    /* Cache the true boot contact state now. While not-yet-joined this only
     * writes s_pending_open; on_joined() applies it whenever the join
     * completes — including the slow path where wait_ready() below times out.
     * Doing it here (not gated on the wait) avoids a stuck-wrong initial
     * report if commissioning takes longer than JOIN_TIMEOUT_MS. */
    zb_router_set_contact(door_contact_is_open());

    if (zb_router_wait_ready(JOIN_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "joined on the fast path");
    } else {
        ESP_LOGW(TAG, "join timeout — stack keeps retrying; on_joined will apply state");
    }

    /* Rollback policy: reaching app_main() means the freshly-OTA'd image booted
     * cleanly, so confirm it ("booted = good"). This gates on boot success, not
     * on Zigbee join. */
    if (ota_client_image_pending_verify()) {
        ota_client_mark_valid();
        ESP_LOGI(TAG, "OTA image confirmed valid");
    }

    /* app_main returns; the Zigbee stack task and esp_timer drive everything. */
}
