/**
 * @file zb_router.c
 * @brief Zigbee ROUTER bring-up + IAS Zone contact reporting + OTA.
 *        esp-zigbee-lib 1.6.x native esp_zb_* API. Router never sleeps.
 */
#ifdef USE_ZIGBEE

#include "zb_router.h"
#include "ias_status.h"
#include "ota_client.h"

#include "esp_zigbee_core.h"
#include "platform/esp_zigbee_platform.h"
#include "nwk/esp_zigbee_nwk.h"
#include "bdb/esp_zigbee_bdb_commissioning.h"
#include "zdo/esp_zigbee_zdo_common.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_endpoint.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_ias_zone.h"
#include "zcl/esp_zigbee_zcl_core.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ZB_RTR";

#define APP_ENDPOINT           1U
#define KEEPALIVE_REPORT_MIN   60U

/* ZCL length-prefixed strings: first byte = character count.
 * "DFRobot-DIY" = 11 chars = 0x0B. "DFR-DoorSensor" = 14 chars = 0x0E. */
#define MANUF_NAME  "\x0B" "DFRobot-DIY"
#define MODEL_ID    "\x0E" "DFR-DoorSensor"

static volatile bool s_joined = false;

/* IAS Zone ZoneStatus (0x0002) storage — the ZCL table stores a pointer to this,
 * so it must live for the stack lifetime. */
static uint16_t s_zone_status = 0;
static volatile bool s_pending_open = false;  /* last requested state (pre/post join) */

/* forward decls */
static void keepalive_cb(uint8_t param);
static void apply_contact_no_lock(bool open);   /* caller must hold the Zigbee lock */

/* Single global core-action dispatcher: OTA + IAS Zone. Only ONE may be
 * registered stack-wide, so it lives here (ota_client's own registration was
 * removed in Task 6). */
static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t cb_id,
                                   const void *message)
{
    switch (cb_id) {
    case ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID:
        return ota_client_on_value(
            (const esp_zb_zcl_ota_upgrade_value_message_t *)message);
    default:
        /* IAS Zone enroll / CIE-write and other events surface here; log for
         * bring-up observability. z2m performs auto-enroll-response. */
        ESP_LOGD(TAG, "unhandled core action cb_id=0x%x", (unsigned)cb_id);
        return ESP_OK;
    }
}

static void steering_alarm_cb(uint8_t mode)
{
    esp_zb_bdb_start_top_level_commissioning(
        (esp_zb_bdb_commissioning_mode_mask_t)mode);
}

static void on_joined(void)
{
    s_joined = true;
    ESP_LOGI(TAG, "joined network, short addr 0x%04hx", esp_zb_get_short_address());
    ota_client_on_joined();
    /* Apply the latest door state now that the network is up, then start the
     * keepalive refresh cycle. We are in Zigbee callback context (the signal
     * handler already holds the Zigbee lock), so use the no-lock apply — calling
     * the lock-taking zb_router_set_contact() here would self-deadlock. */
    apply_contact_no_lock(s_pending_open);
    esp_zb_scheduler_alarm(keepalive_cb, 0, 1000U);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig = (esp_zb_app_signal_type_t)*p_sg_p;

    switch (sig) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "stack ready, starting BDB init");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "factory-new — starting network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                on_joined();  /* rejoined existing network */
            }
        } else {
            ESP_LOGW(TAG, "start/reboot status %d", err);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err == ESP_OK) {
            on_joined();
        } else {
            ESP_LOGW(TAG, "steering failed (%d), retry in 1s", err);
            esp_zb_scheduler_alarm(steering_alarm_cb,
                                   (uint8_t)ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000U);
        }
        break;
    default:
        ESP_LOGI(TAG, "ZDO signal %s (0x%x) status %d",
                 esp_zb_zdo_signal_to_string(sig), (unsigned)sig, err);
        break;
    }
}

/* Write the IAS Zone ZoneStatus attribute WITHOUT taking the Zigbee lock. The
 * caller MUST already hold it — either running in Zigbee stack/callback context
 * (signal handler, scheduler alarm) or wrapped in esp_zb_lock_acquire/release. */
static void apply_contact_no_lock(bool open)
{
    s_zone_status = ias_zone_status_encode(open);
    esp_zb_zcl_set_attribute_val(APP_ENDPOINT,
                                 ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATUS_ID,
                                 &s_zone_status,
                                 false);
    ESP_LOGI(TAG, "ZoneStatus <- %s (0x%04x)", open ? "OPEN" : "CLOSED", s_zone_status);
}

/* Re-apply the current ZoneStatus periodically as a state-recovery keepalive.
 * Runs in Zigbee scheduler/stack context (lock already held) -> no-lock apply. */
static void keepalive_cb(uint8_t param)
{
    (void)param;
    if (s_joined) {
        apply_contact_no_lock(s_pending_open);
    }
    esp_zb_scheduler_alarm(keepalive_cb, 0, KEEPALIVE_REPORT_MIN * 60U * 1000U);
}

/* Public setter for EXTERNAL application tasks (not stack callbacks). Takes the
 * Zigbee lock, then delegates to the no-lock apply. Safe before join: the value
 * is cached in s_pending_open and applied by on_joined() once the network is up. */
void zb_router_set_contact(bool open)
{
    s_pending_open = open;
    if (!s_joined) {
        return;  /* applied from on_joined() once the network is up */
    }
    if (!esp_zb_lock_acquire(portMAX_DELAY)) {
        ESP_LOGE(TAG, "failed to acquire Zigbee lock");
        return;
    }
    apply_contact_no_lock(open);
    esp_zb_lock_release();
}

static void esp_zb_task(void *pv)
{
    (void)pv;

    esp_zb_cfg_t zb_cfg = {
        .esp_zb_role         = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg    = { .max_children = 10 },
    };
    esp_zb_init(&zb_cfg);
    esp_zb_set_rx_on_when_idle(true);   /* router: receiver always on */

    /* Basic cluster (+ manufacturer/model identity for z2m recognition). */
    esp_zb_basic_cluster_cfg_t basic_cfg = { .zcl_version = 8, .power_source = 1 }; /* 0x01 = mains */
    esp_zb_attribute_list_t *basic_attrs = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic_attrs,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)MANUF_NAME);
    esp_zb_basic_cluster_add_attr(basic_attrs,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)MODEL_ID);

    /* Identify cluster. */
    esp_zb_identify_cluster_cfg_t id_cfg = { .identify_time = 0 };
    esp_zb_attribute_list_t *id_attrs = esp_zb_identify_cluster_create(&id_cfg);

    /* IAS Zone cluster (0x0500) as a contact switch. */
    esp_zb_ias_zone_cluster_cfg_t ias_cfg = {
        .zone_state  = ESP_ZB_ZCL_IAS_ZONE_ZONESTATE_NOT_ENROLLED,
        .zone_type   = ESP_ZB_ZCL_IAS_ZONE_ZONETYPE_CONTACT_SWITCH, /* 0x0015 */
        .zone_status = 0,
        .zone_id     = 0,
    };
    esp_zb_attribute_list_t *ias_attrs = esp_zb_ias_zone_cluster_create(&ias_cfg);

    /* Assemble clusters. */
    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(clusters, basic_attrs, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(clusters, id_attrs, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_ias_zone_cluster(clusters, ias_attrs, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    ota_client_add_cluster(clusters);

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint           = APP_ENDPOINT,
        .app_profile_id     = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id      = ESP_ZB_HA_IAS_ZONE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(ep_list, clusters, ep_cfg);
    esp_zb_device_register(ep_list);

    /* Configure device-side reporting on ZoneStatus so the stack pushes reports
     * on change (mirrors the proven MoistureTracker mechanism; the explicit
     * report command asserts in this SDK, so attribute-set + reporting is used). */
    esp_zb_zcl_reporting_info_t rep = {
        .direction      = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .ep             = APP_ENDPOINT,
        .cluster_id     = ESP_ZB_ZCL_CLUSTER_ID_IAS_ZONE,
        .cluster_role   = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .attr_id        = ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATUS_ID,
        .manuf_code     = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC,
        .dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .u.send_info    = { .min_interval = 0, .max_interval = 0,
                            .def_min_interval = 0, .def_max_interval = 0,
                            .delta.u16 = 1 },
    };
    esp_zb_zcl_update_reporting_info(&rep);

    esp_zb_core_action_handler_register(zb_action_handler);  /* shared OTA + IAS */
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);

    ESP_ERROR_CHECK(esp_zb_start(false));
    ota_client_start(APP_ENDPOINT);
    esp_zb_stack_main_loop();  /* never returns */
    vTaskDelete(NULL);
}

esp_err_t zb_router_init(void)
{
    ESP_LOGI(TAG, "init Zigbee router");
    esp_zb_platform_config_t pcfg = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&pcfg));
    BaseType_t ret = xTaskCreate(esp_zb_task, "esp_zb_task", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create Zigbee task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool zb_router_wait_ready(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (!s_joined && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(100));
        waited += 100;
    }
    return s_joined;
}

#endif /* USE_ZIGBEE */
