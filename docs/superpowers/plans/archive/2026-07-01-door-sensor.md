# DFR-DoorSensor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A mains/USB-powered ESP32-C6 Zigbee **Router** that reports a KW11-2N door switch to Zigbee2MQTT as a native IAS Zone contact sensor, with OTA update support.

**Architecture:** ESP-IDF/PlatformIO firmware modeled on the sibling `../DFR-MoistureTracker`. `app_main()` starts once and never sleeps: it initializes the door GPIO (interrupt + debounce timer) and starts the Zigbee stack as a router. A GPIO edge → ISR → debounce one-shot → settled level → logical open/closed → IAS Zone ZoneStatus attribute update → Zigbee report. Pure decision logic (polarity mapping, ZoneStatus encoding, debounce settling) lives in host-testable modules; GPIO/timer/stack wiring lives in ESP-only modules validated on-device.

**Tech Stack:** C, ESP-IDF (via PlatformIO), native `esp_zb_*` API from `esp-zigbee-lib ~1.6.0` + `esp-zboss-lib ~1.6.0`, Unity for host tests.

## Global Constraints

- **Board:** `dfrobot_firebeetle2_esp32c6` (DFRobot FireBeetle 2 ESP32-C6).
- **Zigbee role:** Router — `esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER`, `esp_zb_set_rx_on_when_idle(true)`. **No** deep sleep, **no** light sleep, **no** `CONFIG_PM_ENABLE` / tickless idle. Radio stays on.
- **sdkconfig (zigbee env):** `CONFIG_ZB_ENABLED=y`, `CONFIG_ZB_ZCZR=y` (router library — NOT `CONFIG_ZB_ZED`), `CONFIG_ZB_RADIO_NATIVE=y`, `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`, `CONFIG_PARTITION_TABLE_CUSTOM=y`, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, console on USB-Serial-JTAG.
- **Dependencies:** pin `espressif/esp-zigbee-lib: "~1.6.0"` and `espressif/esp-zboss-lib: "~1.6.0"` (2.x moved to a different native API).
- **Contact hardware:** KW11-2N SPDT snap-action switch — Common → **GPIO5** (internal pull-up), one throw → GND, spare throw unused.
- **Config constants (`src/main.c`):** `DEFAULT_DEVICE_ID "door01"`, `DOOR_CONTACT_GPIO 5`, `DOOR_DEBOUNCE_MS 40`, `KEEPALIVE_REPORT_MIN 60`, `DOOR_CONTACT_ACTIVE_LOW 1`.
- **IAS Zone:** cluster 0x0500, `ZoneType = 0x0015` (contact switch); ZoneStatus **bit 0 (alarm1) = contact open**. No custom z2m converter — naming via z2m `friendly_name`.
- **Naming:** functions `module_verb_noun()`, constants `UPPER_SNAKE_CASE`, types `snake_case_t`, log tags short uppercase (e.g. `"DOOR"`).
- **Design-for-extraction:** `zb_router`, `ias_zone`, `ota_client` keep narrow, role-agnostic interfaces (future `esp-zb-common` library — separate spec). Do not build that library here.
- **Partitions:** exact dual-OTA layout from Task 1 (offsets pinned).

---

## File Structure

- `platformio.ini` — build envs (zigbee, zigbee_test, native).
- `partitions.csv` — dual-OTA + Zigbee NVRAM layout.
- `sdkconfig.defaults`, `sdkconfig.defaults.zigbee` — base + router Zigbee config.
- `src/idf_component.yml` — esp-zigbee-lib / esp-zboss-lib pins.
- `src/CMakeLists.txt` — source list + component REQUIRES.
- `src/main.c` — app entry, init order, config constants, wiring.
- `src/contact_map.c` + `include/contact_map.h` — GPIO level → logical open/closed (pure).
- `src/ias_status.c` + `include/ias_status.h` — logical open/closed → ZoneStatus bits (pure).
- `src/debounce.c` + `include/debounce.h` — debounce settling state machine (pure).
- `src/door_contact.c` + `include/door_contact.h` — GPIO + ISR + esp_timer, uses the three pure modules.
- `src/zb_router.c` + `include/zb_router.h` — Zigbee router bring-up, IAS Zone + OTA clusters, action dispatcher, keepalive.
- `src/ota_client.c` + `include/ota_client.h` — Zigbee OTA download + rollback (ported from MoistureTracker).
- `test/test_contact_map/`, `test/test_ias_status/`, `test/test_debounce/`, `test/test_smoke/` — Unity host tests.

---

## Task 1: Project scaffolding + host test harness

**Files:**
- Create: `platformio.ini`, `partitions.csv`, `sdkconfig.defaults`, `sdkconfig.defaults.zigbee`, `src/idf_component.yml`, `src/CMakeLists.txt`, `src/main.c`, `test/test_smoke/test_smoke.c`

**Interfaces:**
- Consumes: nothing.
- Produces: a buildable skeleton and a working `pio test -e native` harness for later tasks.

- [ ] **Step 1: Create `platformio.ini`**

```ini
; PlatformIO Project Configuration — DFR-DoorSensor
; ESP32-C6 Zigbee-router door sensor (IAS Zone contact).

[platformio]
default_envs = dfrobot_firebeetle2_esp32c6_zigbee

[env]
platform = espressif32
framework = espidf
monitor_speed = 115200
board_build.partitions = partitions.csv

[env:dfrobot_firebeetle2_esp32c6_zigbee]
board = dfrobot_firebeetle2_esp32c6
; Set to your board's serial device before flashing:
; upload_port = /dev/cu.usbmodemXXXX
; monitor_port = /dev/cu.usbmodemXXXX
build_flags = -DUSE_ZIGBEE
; Pure-espidf platform: inject sdkconfig defaults directly so the Zigbee config
; survives a clean/regenerated sdkconfig (same mechanism as DFR-MoistureTracker).
board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.zigbee"

; Identical to the deployment env (router never sleeps) — kept as a named bench
; env for symmetry with MoistureTracker and future per-env tweaks.
[env:dfrobot_firebeetle2_esp32c6_zigbee_test]
extends = env:dfrobot_firebeetle2_esp32c6_zigbee

[env:native]
platform = native
framework =
test_framework = unity
build_flags = -std=c11 -Wall -Wextra -I include
; Host env is test-only. Tests #include the SUT .c directly; exclude src/ from
; the regular build so `pio run` (no -e) never compiles ESP-IDF code on the host.
build_src_filter = -<*>
test_filter =
    test_smoke
```

- [ ] **Step 2: Create `partitions.csv` (verbatim dual-OTA layout)**

```csv
# Name,   Type, SubType, Offset,   Size, Flags
# Explicit offsets: with blank offsets PlatformIO's upload-offset parser diverges
# from ESP-IDF's app-alignment. Pinning offsets keeps both toolchains in agreement.
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
otadata,  data, ota,     0x10000,  0x2000,
ota_0,    app,  ota_0,   0x20000,  0x180000,
ota_1,    app,  ota_1,   0x1a0000, 0x180000,
storage,  data, nvs,     0x320000, 0x4000,
zb_storage, data, fat,   0x324000, 0x4000,
zb_fct,   data, fat,     0x328000, 0x400,
```

- [ ] **Step 3: Create `sdkconfig.defaults`**

```
CONFIG_NVS_FLASH=y
# Route console output to the built-in USB-Serial-JTAG so logs appear over USB.
CONFIG_ESP_CONSOLE_UART_DEFAULT=n
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```

- [ ] **Step 4: Create `sdkconfig.defaults.zigbee`**

```
# Base console-over-USB (mirrors sdkconfig.defaults)
CONFIG_NVS_FLASH=y
CONFIG_ESP_CONSOLE_UART_DEFAULT=n
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y

# Zigbee ROUTER on the native 802.15.4 radio.
CONFIG_ZB_ENABLED=y
CONFIG_ZB_ZCZR=y
CONFIG_ZB_RADIO_NATIVE=y

# Flash size (board is 4MB; explicit because this env's defaults replace the base).
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y

# Custom partition table (adds zb_storage / zb_fct + dual OTA).
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# OTA: roll back to the previous app if a freshly-flashed image fails its self-check.
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

Note: **do NOT** add `CONFIG_PM_ENABLE`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE`, or `CONFIG_ESP_PHY_MAC_BB_PD` — a router must stay awake.

- [ ] **Step 5: Create `src/idf_component.yml`**

```yaml
dependencies:
  # Pin to 1.6.x: esp_zb_* native API matched pair with esp-zboss-lib 1.6.x.
  # The 2.x line moved to a different native API.
  espressif/esp-zigbee-lib: "~1.6.0"
  espressif/esp-zboss-lib: "~1.6.0"
```

- [ ] **Step 6: Create `src/CMakeLists.txt` (only main.c for now; grows per task)**

```cmake
set(SRCS
    "main.c"
)

# Managed-component include path, added unconditionally so it resolves in the
# zigbee env. In a non-zigbee build the Zigbee bodies are gated by #ifdef USE_ZIGBEE.
set(ZB_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp-zigbee-lib/include"
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "."
        "../include"
        ${ZB_INCLUDE_DIR}
    REQUIRES
        espressif__esp-zigbee-lib
        espressif__esp-zboss-lib
        esp_timer
        nvs_flash
        esp_event
)
```

- [ ] **Step 7: Create minimal `src/main.c`**

```c
/**
 * @file main.c
 * @brief DFR-DoorSensor entry point. ESP32-C6 Zigbee-router door sensor.
 *        Wiring is filled in across the implementation plan tasks.
 */
#include "esp_log.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "DFR-DoorSensor boot (scaffold)");
}
```

- [ ] **Step 8: Create `test/test_smoke/test_smoke.c`**

```c
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_harness_runs(void) { TEST_ASSERT_TRUE(1); }

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_harness_runs);
    return UNITY_END();
}
```

- [ ] **Step 9: Run the host test to verify the harness works**

Run: `pio test -e native`
Expected: PASS — `test_harness_runs` passes, `1 Tests 0 Failures`.

- [ ] **Step 10: Commit**

```bash
git add platformio.ini partitions.csv sdkconfig.defaults sdkconfig.defaults.zigbee src include test
git commit -m "chore: project scaffolding + native test harness"
```

---

## Task 2: Contact polarity mapping (pure, TDD)

**Files:**
- Create: `include/contact_map.h`, `src/contact_map.c`, `test/test_contact_map/test_contact_map.c`
- Modify: `platformio.ini` (add `test_contact_map` to native `test_filter`)

**Interfaces:**
- Consumes: nothing.
- Produces: `bool contact_is_open(int gpio_level, bool active_low)` — maps a raw GPIO level (0/1) to logical door-open. When `active_low` is true, level 0 means open.

- [ ] **Step 1: Write the failing test — `test/test_contact_map/test_contact_map.c`**

```c
#include <unity.h>
#include "../../src/contact_map.c"

void setUp(void) {}
void tearDown(void) {}

// active_low = true: LOW level (0) == door open
static void test_active_low_low_is_open(void)   { TEST_ASSERT_TRUE(contact_is_open(0, true)); }
static void test_active_low_high_is_closed(void){ TEST_ASSERT_FALSE(contact_is_open(1, true)); }
// active_low = false: HIGH level (1) == door open
static void test_active_high_high_is_open(void) { TEST_ASSERT_TRUE(contact_is_open(1, false)); }
static void test_active_high_low_is_closed(void){ TEST_ASSERT_FALSE(contact_is_open(0, false)); }
// any nonzero level counts as HIGH
static void test_nonzero_level_is_high(void)    { TEST_ASSERT_FALSE(contact_is_open(5, true)); }

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_active_low_low_is_open);
    RUN_TEST(test_active_low_high_is_closed);
    RUN_TEST(test_active_high_high_is_open);
    RUN_TEST(test_active_high_low_is_closed);
    RUN_TEST(test_nonzero_level_is_high);
    return UNITY_END();
}
```

- [ ] **Step 2: Add `test_contact_map` to the native `test_filter` in `platformio.ini`**

Under `[env:native]`, extend `test_filter`:

```ini
test_filter =
    test_smoke
    test_contact_map
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `pio test -e native -f test_contact_map`
Expected: FAIL to compile — `contact_map.c` / `contact_is_open` not found.

- [ ] **Step 4: Create `include/contact_map.h`**

```c
#ifndef CONTACT_MAP_H
#define CONTACT_MAP_H

#include <stdbool.h>

/* Map a raw GPIO level to logical door-open.
 * active_low == true : a LOW level (0) means the door is OPEN.
 * active_low == false: a HIGH level (nonzero) means the door is OPEN. */
bool contact_is_open(int gpio_level, bool active_low);

#endif // CONTACT_MAP_H
```

- [ ] **Step 5: Create `src/contact_map.c`**

```c
#include "contact_map.h"

bool contact_is_open(int gpio_level, bool active_low)
{
    bool high = (gpio_level != 0);
    return active_low ? !high : high;
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `pio test -e native -f test_contact_map`
Expected: PASS — 5 tests, 0 failures.

- [ ] **Step 7: Commit**

```bash
git add include/contact_map.h src/contact_map.c test/test_contact_map platformio.ini
git commit -m "feat: contact polarity mapping (pure)"
```

---

## Task 3: IAS ZoneStatus encoding (pure, TDD)

**Files:**
- Create: `include/ias_status.h`, `src/ias_status.c`, `test/test_ias_status/test_ias_status.c`
- Modify: `platformio.ini` (add `test_ias_status` to native `test_filter`)

**Interfaces:**
- Consumes: nothing.
- Produces: `uint16_t ias_zone_status_encode(bool open)` — returns the IAS Zone ZoneStatus bitmap; bit 0 (alarm1, `0x0001`) set when open, 0 when closed. `IAS_ZONE_STATUS_ALARM1` constant exported.

- [ ] **Step 1: Write the failing test — `test/test_ias_status/test_ias_status.c`**

```c
#include <unity.h>
#include "../../src/ias_status.c"

void setUp(void) {}
void tearDown(void) {}

static void test_open_sets_alarm1(void)     { TEST_ASSERT_EQUAL_UINT16(0x0001, ias_zone_status_encode(true)); }
static void test_closed_clears_status(void) { TEST_ASSERT_EQUAL_UINT16(0x0000, ias_zone_status_encode(false)); }
static void test_alarm1_constant(void)      { TEST_ASSERT_EQUAL_UINT16(0x0001, IAS_ZONE_STATUS_ALARM1); }

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_open_sets_alarm1);
    RUN_TEST(test_closed_clears_status);
    RUN_TEST(test_alarm1_constant);
    return UNITY_END();
}
```

- [ ] **Step 2: Add `test_ias_status` to the native `test_filter` in `platformio.ini`**

```ini
test_filter =
    test_smoke
    test_contact_map
    test_ias_status
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `pio test -e native -f test_ias_status`
Expected: FAIL to compile — `ias_zone_status_encode` not found.

- [ ] **Step 4: Create `include/ias_status.h`**

```c
#ifndef IAS_STATUS_H
#define IAS_STATUS_H

#include <stdint.h>
#include <stdbool.h>

/* IAS Zone ZoneStatus attribute (0x0002) bitmap. */
#define IAS_ZONE_STATUS_ALARM1 ((uint16_t)0x0001)

/* Encode logical door state into the ZoneStatus bitmap.
 * open == true  -> alarm1 bit set (contact open)
 * open == false -> 0 (contact closed). */
uint16_t ias_zone_status_encode(bool open);

#endif // IAS_STATUS_H
```

- [ ] **Step 5: Create `src/ias_status.c`**

```c
#include "ias_status.h"

uint16_t ias_zone_status_encode(bool open)
{
    return open ? IAS_ZONE_STATUS_ALARM1 : (uint16_t)0;
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `pio test -e native -f test_ias_status`
Expected: PASS — 3 tests, 0 failures.

- [ ] **Step 7: Commit**

```bash
git add include/ias_status.h src/ias_status.c test/test_ias_status platformio.ini
git commit -m "feat: IAS Zone ZoneStatus encoding (pure)"
```

---

## Task 4: Debounce settling state machine (pure, TDD)

**Files:**
- Create: `include/debounce.h`, `src/debounce.c`, `test/test_debounce/test_debounce.c`
- Modify: `platformio.ini` (add `test_debounce` to native `test_filter`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `typedef struct { int stable_level; } debounce_t;`
  - `void debounce_init(debounce_t *d, int initial_level);`
  - `bool debounce_settle(debounce_t *d, int sampled_level);` — call when the debounce timer expires with the freshly-sampled level. Returns true and updates `stable_level` **only** when the sampled level differs from the last stable level (a real transition); returns false otherwise.
  - `int debounce_stable_level(const debounce_t *d);`

The edge-triggered timer restart is pure hardware timing (Task 5); this module is the testable "did the level actually change once it settled" decision.

- [ ] **Step 1: Write the failing test — `test/test_debounce/test_debounce.c`**

```c
#include <unity.h>
#include "../../src/debounce.c"

void setUp(void) {}
void tearDown(void) {}

static void test_init_sets_stable_level(void)
{
    debounce_t d;
    debounce_init(&d, 1);
    TEST_ASSERT_EQUAL_INT(1, debounce_stable_level(&d));
}

static void test_settle_same_level_no_change(void)
{
    debounce_t d;
    debounce_init(&d, 1);
    TEST_ASSERT_FALSE(debounce_settle(&d, 1));
    TEST_ASSERT_EQUAL_INT(1, debounce_stable_level(&d));
}

static void test_settle_different_level_reports_change(void)
{
    debounce_t d;
    debounce_init(&d, 1);
    TEST_ASSERT_TRUE(debounce_settle(&d, 0));
    TEST_ASSERT_EQUAL_INT(0, debounce_stable_level(&d));
}

static void test_settle_change_then_same_no_second_change(void)
{
    debounce_t d;
    debounce_init(&d, 1);
    TEST_ASSERT_TRUE(debounce_settle(&d, 0));   // 1 -> 0 : change
    TEST_ASSERT_FALSE(debounce_settle(&d, 0));  // 0 -> 0 : no change
}

static void test_bounce_back_to_original_no_change(void)
{
    debounce_t d;
    debounce_init(&d, 1);
    // Chatter that settles back to the original level yields no reported change.
    TEST_ASSERT_FALSE(debounce_settle(&d, 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_sets_stable_level);
    RUN_TEST(test_settle_same_level_no_change);
    RUN_TEST(test_settle_different_level_reports_change);
    RUN_TEST(test_settle_change_then_same_no_second_change);
    RUN_TEST(test_bounce_back_to_original_no_change);
    return UNITY_END();
}
```

- [ ] **Step 2: Add `test_debounce` to the native `test_filter` in `platformio.ini`**

```ini
test_filter =
    test_smoke
    test_contact_map
    test_ias_status
    test_debounce
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `pio test -e native -f test_debounce`
Expected: FAIL to compile — `debounce_init` / `debounce_settle` not found.

- [ ] **Step 4: Create `include/debounce.h`**

```c
#ifndef DEBOUNCE_H
#define DEBOUNCE_H

#include <stdbool.h>

/* Debounce settling: the ISR restarts a one-shot timer on every edge; when the
 * timer finally expires the settled GPIO level is passed to debounce_settle().
 * This module decides whether that settled level is a real transition. */
typedef struct {
    int stable_level;
} debounce_t;

void debounce_init(debounce_t *d, int initial_level);

/* Returns true (and updates stable_level) only when sampled_level differs from
 * the current stable level. Returns false when the level is unchanged. */
bool debounce_settle(debounce_t *d, int sampled_level);

int debounce_stable_level(const debounce_t *d);

#endif // DEBOUNCE_H
```

- [ ] **Step 5: Create `src/debounce.c`**

```c
#include "debounce.h"

void debounce_init(debounce_t *d, int initial_level)
{
    d->stable_level = initial_level;
}

bool debounce_settle(debounce_t *d, int sampled_level)
{
    if (sampled_level == d->stable_level) {
        return false;
    }
    d->stable_level = sampled_level;
    return true;
}

int debounce_stable_level(const debounce_t *d)
{
    return d->stable_level;
}
```

- [ ] **Step 6: Run the test to verify it passes**

Run: `pio test -e native -f test_debounce`
Expected: PASS — 5 tests, 0 failures.

- [ ] **Step 7: Commit**

```bash
git add include/debounce.h src/debounce.c test/test_debounce platformio.ini
git commit -m "feat: debounce settling state machine (pure)"
```

---

## Task 5: `door_contact` GPIO/ISR/timer module (on-device)

**Files:**
- Create: `include/door_contact.h`, `src/door_contact.c`
- Modify: `src/CMakeLists.txt` (add sources), `src/main.c` (temporary demo wiring for this task's verification)

**Interfaces:**
- Consumes: `contact_is_open()` (Task 2), `debounce_*()` (Task 4).
- Produces:
  - `typedef void (*door_contact_cb_t)(bool open);`
  - `esp_err_t door_contact_init(int gpio, uint32_t debounce_ms, bool active_low, door_contact_cb_t cb);` — configures the GPIO as input with internal pull-up, installs a both-edge ISR that restarts a one-shot `esp_timer`; on expiry it samples the level, runs debounce + polarity mapping, and invokes `cb(open)` on a real transition. `cb` runs in `esp_timer`'s callback task context (safe to take the Zigbee lock).
  - `bool door_contact_is_open(void);` — current debounced logical state.

This module cannot be host-tested (GPIO/ISR/timer); its pure decision logic already has coverage in Tasks 2 and 4. Validation is on-device via serial logs.

- [ ] **Step 1: Create `include/door_contact.h`**

```c
#ifndef DOOR_CONTACT_H
#define DOOR_CONTACT_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef void (*door_contact_cb_t)(bool open);

/* Configure the door switch GPIO (input, internal pull-up), install a both-edge
 * ISR + debounce one-shot timer, and register the state-change callback.
 * The callback fires once per settled transition, in esp_timer task context. */
esp_err_t door_contact_init(int gpio, uint32_t debounce_ms, bool active_low,
                            door_contact_cb_t cb);

/* Current debounced logical door-open state. */
bool door_contact_is_open(void);

#endif // DOOR_CONTACT_H
```

- [ ] **Step 2: Create `src/door_contact.c`**

```c
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
```

- [ ] **Step 3: Add sources to `src/CMakeLists.txt`**

Update the `SRCS` list to:

```cmake
set(SRCS
    "main.c"
    "contact_map.c"
    "debounce.c"
    "door_contact.c"
)
```

- [ ] **Step 4: Add temporary demo wiring to `src/main.c`**

Replace the body of `app_main` with:

```c
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
```

- [ ] **Step 5: Build the firmware**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: build SUCCESS (managed components download on first build).

- [ ] **Step 6: Flash and verify on-device**

Set `upload_port`/`monitor_port` in `platformio.ini`, then run:
`pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor`
Expected serial: `door_contact init: gpio=5 ... start=CLOSED` (or OPEN per wiring). Toggle the KW11-2N lever: each actuation logs exactly one `door OPEN` / `door CLOSED` + the `callback:` line. Rapid taps must not produce duplicate transitions (debounce working).

- [ ] **Step 7: Commit**

```bash
git add include/door_contact.h src/door_contact.c src/CMakeLists.txt src/main.c
git commit -m "feat: door_contact GPIO/ISR/debounce module"
```

---

## Task 6: Port the OTA client

> **Revised during execution:** the reference `ota_client.c` depends on three
> MoistureTracker headers (`ota_ids.h`, `fw_version.h`, `zigbee_reporter.h`) that
> the naive "copy verbatim" framing missed. This task now creates the two OTA
> identity/version headers with **door-sensor** values and **decouples** the port
> from the reporter (the door sensor has no ADC/e-paper reporting to pause during
> OTA). Decoupling also keeps `ota_client` reporter-agnostic — better for the
> future `esp-zb-common` extraction.
>
> **Further ED-ism removals (surfaced at full link in Task 8):** the reference OTA
> burst helpers also managed the receiver for a sleepy end-device —
> `esp_zb_sleep_enable()` (absent from the router `zczr` lib, a hard link error)
> and `esp_zb_set_rx_on_when_idle(true/false)`. A router keeps `rx_on_when_idle`
> true at all times (set once in `zb_router`), and restoring it to `false` on OTA
> abort/error would silently kill the router's receiver. Both were removed from
> `ota_client.c`; the OTA burst now only arms/disarms the stall watchdog.

**Files:**
- Create: `include/ota_ids.h`, `include/fw_version.h`, `include/ota_client.h`, `src/ota_client.c`
- Modify: `src/CMakeLists.txt` (add `ota_client.c`)

**Interfaces:**
- Consumes: esp-zigbee OTA API.
- Produces (used by Task 7):
  - `esp_err_t ota_client_on_value(const esp_zb_zcl_ota_upgrade_value_message_t *msg);`
  - `void ota_client_add_cluster(esp_zb_cluster_list_t *clusters);`
  - `void ota_client_start(uint8_t endpoint);` — records the client endpoint + query interval. Its core-action-handler registration is removed (Step 4), because `zb_router` owns the single global dispatcher.
  - `void ota_client_on_joined(void);`
  - `bool ota_client_image_pending_verify(void);`
  - `void ota_client_mark_valid(void);`

- [ ] **Step 1: Create `include/ota_ids.h` with door-sensor OTA identity**

These three values MUST match across the firmware, the `.ota` image header, and
the z2m OTA index. Manufacturer code stays the shared DIY `0xFEFE`; image type is
`0x0002` (the soil sensor is `0x0001`) so z2m never cross-serves soil firmware;
model id matches the Basic-cluster model string used in Task 7.

```c
#ifndef OTA_IDS_H
#define OTA_IDS_H

#include <stdint.h>

/* Fixed product identity for Zigbee OTA. These three values MUST match across
 * the firmware, the .ota image header, and the z2m OTA index. */
#define OTA_MANUFACTURER_CODE  0xFEFEu   /* shared DIY 16-bit code */
#define OTA_IMAGE_TYPE         0x0002u   /* door sensor (soil sensor is 0x0001) */
#define OTA_MODEL_ID           "DFR-DoorSensor"

/* Pack semver into the 32-bit Zigbee fileVersion as 0xMMmmpp bb
 * (major, minor, patch, build). Monotonic: higher semver => larger uint32. */
#define OTA_PACK_VERSION(major, minor, patch, build)            \
    (((uint32_t)(major) << 24) | ((uint32_t)(minor) << 16) |    \
     ((uint32_t)(patch) << 8)  |  (uint32_t)(build))

#endif /* OTA_IDS_H */
```

- [ ] **Step 2: Copy `include/fw_version.h` verbatim (product-agnostic)**

Run: `cp ../DFR-MoistureTracker/include/fw_version.h include/fw_version.h`
No edits — it only defines version macros and includes `ota_ids.h`.

- [ ] **Step 3: Copy the OTA client header + implementation**

Run:
```bash
cp ../DFR-MoistureTracker/include/ota_client.h include/ota_client.h   # transport-agnostic; no edits
cp ../DFR-MoistureTracker/src/ota_client.c src/ota_client.c
```

- [ ] **Step 4: Edit the ported `src/ota_client.c` — three removals**

Make exactly these changes; keep everything else (`ota_client_on_value`,
`ota_client_add_cluster`, endpoint capture, query arming, rollback helpers) intact:

1. **Remove the shared-handler registration.** In `ota_client_start()`, delete the
   single line `esp_zb_core_action_handler_register(zb_action_handler);`. `zb_router`
   (Task 7) registers the one global handler and dispatches OTA via the public
   `ota_client_on_value()`.
2. **Delete the now-unused static `zb_action_handler`** function (the small static
   that switches on `cb_id == ESP_ZB_CORE_OTA_UPGRADE_VALUE_CB_ID` and returns
   `ota_client_on_value(...)`).
3. **Decouple from the reporter.** Delete `#include "zigbee_reporter.h"` and the two
   calls `zigbee_reporter_set_reports_paused(true)` / `...(false)` (in the OTA
   burst begin/end paths). The door sensor has no frequent reporting to pause, so
   the pause is a no-op here; removing it keeps `ota_client` reporter-agnostic.

If any deletion leaves an unused include or forward declaration that warns, remove
just that. Do not otherwise touch includes.

- [ ] **Step 5: Add `ota_client.c` to `src/CMakeLists.txt`**

```cmake
set(SRCS
    "main.c"
    "contact_map.c"
    "debounce.c"
    "door_contact.c"
    "ota_client.c"
)
```

- [ ] **Step 6: Build to confirm the port compiles**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: build SUCCESS, no new warnings from the edits. `ota_client_add_cluster`/
`ota_client_start` are not called by anything yet (zb_router arrives in Task 7) —
they only need to compile. End-to-end OTA is validated on-device in Task 9.

- [ ] **Step 7: Commit**

```bash
git add include/ota_ids.h include/fw_version.h include/ota_client.h src/ota_client.c src/CMakeLists.txt
git commit -m "chore: port Zigbee OTA client (door-sensor identity, reporter-decoupled, handler deferred to zb_router)"
```

---

## Task 7: `zb_router` — Zigbee router bring-up + IAS Zone + shared dispatcher (on-device)

> **Revised during execution (deadlock fix):** the original Step 3/4 code called the
> lock-taking `zb_router_set_contact()` from `on_joined()` and `keepalive_cb()`, which
> both run in Zigbee callback context where the stack lock is already held —
> `esp_zb_lock_acquire()` there self-deadlocks the stack task on every join. Split into
> a no-lock `apply_contact_no_lock()` (for stack-context callers) and the lock-taking
> public `zb_router_set_contact()` (for external app tasks), mirroring the reference
> project's `update_attributes_no_lock` / `zigbee_reporter_report` split.

**Files:**
- Create: `include/zb_router.h`, `src/zb_router.c`
- Modify: `src/CMakeLists.txt` (add `zb_router.c`)

**Interfaces:**
- Consumes: `ias_zone_status_encode()` (Task 3), `ota_client_*()` (Task 6).
- Produces (used by Task 8):
  - `esp_err_t zb_router_init(void);` — configures the native radio, creates the stack task; join is asynchronous.
  - `bool zb_router_wait_ready(uint32_t timeout_ms);` — blocks until joined or timeout.
  - `void zb_router_set_contact(bool open);` — sets the IAS Zone ZoneStatus attribute from any task (takes the Zigbee lock). Safe to call before join (value is cached and applied post-join).

This is the router adaptation of MoistureTracker's `zigbee_reporter.c`. Key differences from that reference: **router role** (`ESP_ZB_DEVICE_TYPE_ROUTER`, `esp_zb_set_rx_on_when_idle(true)`), **no sleep** (no `esp_pm`, no `CAN_SLEEP`, no sleep threshold), **IAS Zone cluster** instead of humidity/power, and a **shared action dispatcher** covering OTA + IAS Zone.

- [ ] **Step 1: Create `include/zb_router.h`**

```c
#ifndef ZB_ROUTER_H
#define ZB_ROUTER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Start the Zigbee stack as a Router (radio always on). Restores network state
 * if already joined, else begins BDB steering. Returns once the stack task is
 * running; join completes asynchronously. */
esp_err_t zb_router_init(void);

/* Block until joined or the timeout elapses. Returns true if joined. */
bool zb_router_wait_ready(uint32_t timeout_ms);

/* Set the IAS Zone ZoneStatus from the current door state. Takes the Zigbee
 * lock; callable from any FreeRTOS task and before join (value is cached). */
void zb_router_set_contact(bool open);

#endif // ZB_ROUTER_H
```

- [ ] **Step 2: Create `src/zb_router.c` — includes, state, config**

```c
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
```

- [ ] **Step 3: Add the shared action dispatcher + signal handler**

Append to `src/zb_router.c`:

```c
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
```

- [ ] **Step 4: Add the keepalive callback + contact setter**

Append to `src/zb_router.c`:

Two-context split (mirrors the reference project's `update_attributes_no_lock` /
`zigbee_reporter_report` pattern). `apply_contact_no_lock()` writes the ZoneStatus
attribute **without** taking the Zigbee lock — the caller must already hold it.
Stack-context callers (`on_joined`, `keepalive_cb`, both run from Zigbee callbacks
that already hold the lock) call it directly. The public `zb_router_set_contact()`,
invoked from external application tasks (e.g. the door_contact `esp_timer` callback),
takes the lock and then delegates. Acquiring the lock from a context that already
holds it would self-deadlock the stack task, so the two paths must stay distinct.

```c
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
```

- [ ] **Step 5: Add the stack task (endpoint + IAS Zone cluster) + public init**

Append to `src/zb_router.c`:

```c
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
```

- [ ] **Step 6: Add `zb_router.c` to `src/CMakeLists.txt`**

```cmake
set(SRCS
    "main.c"
    "contact_map.c"
    "debounce.c"
    "door_contact.c"
    "ota_client.c"
    "zb_router.c"
)
```

- [ ] **Step 7: Build**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: build SUCCESS.

**API-name caveat (resolve at build time, do not guess silently):** the exact IAS Zone symbol names (`esp_zb_ias_zone_cluster_cfg_t`, `esp_zb_ias_zone_cluster_create`, `esp_zb_cluster_list_add_ias_zone_cluster`, `ESP_ZB_ZCL_ATTR_IAS_ZONE_ZONESTATUS_ID`, `ESP_ZB_ZCL_IAS_ZONE_ZONETYPE_CONTACT_SWITCH`, `ESP_ZB_HA_IAS_ZONE_ID`) must match the installed `managed_components/espressif__esp-zigbee-lib/include/zcl/esp_zigbee_zcl_ias_zone.h`. If a name mismatches, `grep` that header for the correct symbol and adjust — the semantics above are correct; only spellings may differ across 1.6.x point releases.

- [ ] **Step 8: Commit**

```bash
git add include/zb_router.h src/zb_router.c src/CMakeLists.txt
git commit -m "feat: Zigbee router bring-up + IAS Zone contact cluster + shared dispatcher"
```

---

## Task 8: Wire `main.c` end-to-end (on-device)

**Files:**
- Modify: `src/main.c`

**Interfaces:**
- Consumes: `door_contact_init()` (Task 5), `zb_router_init/wait_ready/set_contact()` (Task 7), `ota_client_image_pending_verify/mark_valid()` (Task 6).
- Produces: the complete application.

- [ ] **Step 1: Replace `src/main.c` with the full application**

```c
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
#define DOOR_CONTACT_ACTIVE_LOW  1
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

    if (zb_router_wait_ready(JOIN_TIMEOUT_MS)) {
        ESP_LOGI(TAG, "joined; pushing initial door state");
        zb_router_set_contact(door_contact_is_open());
    } else {
        ESP_LOGW(TAG, "join timeout — stack keeps retrying in background");
    }

    /* Confirm a freshly-OTA'd image so the bootloader keeps it (no-op otherwise). */
    if (ota_client_image_pending_verify()) {
        ota_client_mark_valid();
        ESP_LOGI(TAG, "OTA image confirmed valid");
    }

    /* app_main returns; the Zigbee stack task and esp_timer drive everything. */
}
```

- [ ] **Step 2: Build**

Run: `pio run -e dfrobot_firebeetle2_esp32c6_zigbee`
Expected: build SUCCESS.

- [ ] **Step 3: Erase + flash (first pairing needs a clean NVS)**

Run:
```bash
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t erase
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor
```
Expected serial: `boot (door01)` → `stack ready` → `factory-new — starting network steering` → (with z2m permit-join on) `joined network, short addr 0x...` → `ZoneStatus <- CLOSED/OPEN`.

- [ ] **Step 4: Commit**

```bash
git add src/main.c
git commit -m "feat: wire door_contact + zb_router + OTA rollback in main"
```

---

## Task 9: End-to-end z2m validation + docs

**Files:**
- Create: `README.md`
- Modify: none (validation task)

**Interfaces:**
- Consumes: the complete firmware.
- Produces: a verified device + a short README with the pairing/verification checklist.

- [ ] **Step 1: Pair and verify in Zigbee2MQTT**

With the device flashed (Task 8) and z2m permit-join enabled:
- Confirm z2m interviews the device and shows it as **Router** (not End Device).
- Confirm it is exposed as a **contact** sensor (IAS Zone recognized natively — no custom converter).
- Confirm the IAS Zone shows **enrolled** (z2m sends the auto-enroll-response). If it does not enroll, capture the `unhandled core action cb_id=0x..` debug lines and the z2m log; the fix lives in `zb_router`'s dispatcher (handle the IAS Zone enroll-request callback) — iterate there.

- [ ] **Step 2: Verify live contact reporting**

Actuate the KW11-2N lever. Expected: z2m `contact` flips `true`/`false` within ~1s of each debounced transition, matching the serial `ZoneStatus <- OPEN/CLOSED` lines. Confirm the polarity is correct; if inverted, flip `DOOR_CONTACT_ACTIVE_LOW` in `main.c`, rebuild, reflash.

- [ ] **Step 3: Verify router meshing**

Pair a separate battery end-device (or move one) so it routes through this device; confirm in z2m's network map that traffic relays through the door sensor. (Confirms `rx_on_when_idle` router behavior.)

- [ ] **Step 4: Verify the 60-min keepalive (optional / time-boxed)**

Leave the device idle; confirm a `ZoneStatus <-` refresh line appears ~60 min later (or temporarily lower `KEEPALIVE_REPORT_MIN` to 1 for a quick check, then restore to 60 and reflash).

- [ ] **Step 5: Write `README.md`**

```markdown
# DFR-DoorSensor

ESP32-C6 (DFRobot FireBeetle 2) mains/USB-powered **Zigbee Router** door sensor.
A KW11-2N snap-action switch is reported to Zigbee2MQTT as a native **IAS Zone
contact** sensor. Because it joins as a router, it also relays traffic and
extends the mesh. Supports Zigbee OTA with rollback.

See [CLAUDE.md](CLAUDE.md) for architecture and
[docs/superpowers/specs/2026-07-01-door-sensor-design.md](docs/superpowers/specs/2026-07-01-door-sensor-design.md)
for the design spec.

## Build & flash

```bash
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor
pio test -e native   # host unit tests
```

## Wiring

KW11-2N SPDT: **Common → GPIO5** (internal pull-up), one throw → GND, spare throw
unused. Polarity is set by `DOOR_CONTACT_ACTIVE_LOW` in `src/main.c`.

## Pairing

1. Enable permit-join in Zigbee2MQTT.
2. `pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t erase -t upload -t monitor`.
3. z2m interviews it as a Router exposing a `contact` sensor. Rename via
   `friendly_name` in the z2m UI.
```

- [ ] **Step 6: Commit**

```bash
git add README.md
git commit -m "docs: add README with pairing + verification checklist"
```

---

## Self-Review Notes

- **Spec coverage:** router role (Global Constraints, Task 7) ✓; IAS Zone contact / ZoneType 0x0015 / bit0 (Task 3, Task 7) ✓; instant debounced reporting (Tasks 4/5/7) ✓; 60-min keepalive (Task 7 `keepalive_cb`) ✓; OTA + rollback + partitions (Tasks 1/6/8) ✓; GPIO5 + pull-up + polarity flag (Tasks 2/5) ✓; no config portal / converter / battery / display ✓; compile-time name + z2m rename (Task 8, Task 9) ✓; design-for-extraction seams (`zb_router`/`ias_zone`/`ota_client` narrow interfaces) ✓; host tests for the pure logic (Tasks 2–4) ✓.
- **Uncertain-API honesty:** IAS Zone symbol spellings and the enroll-callback path are flagged for build-time/on-device verification (Task 7 Step 7, Task 9 Step 1) rather than presented as guaranteed — the semantics are pinned; only exact 1.6.x spellings may vary.
- **Type consistency:** `door_contact_cb_t`/`door_contact_init`/`door_contact_is_open`, `zb_router_init/wait_ready/set_contact`, `contact_is_open`, `debounce_init/settle/stable_level`, `ias_zone_status_encode` are used identically across producer and consumer tasks.
```
