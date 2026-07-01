# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-C6 IoT firmware (C / ESP-IDF via PlatformIO) for a **mains/USB-powered door
sensor that also acts as a Zigbee Router**. A KW11-2N snap-action microswitch on a
GPIO is debounced and reported to Zigbee2MQTT as a standard **IAS Zone contact sensor**
(cluster 0x0500, ZoneType 0x0015). Because it joins as a router — not a sleepy
end-device — it stays awake, relays traffic, and extends the mesh.

**Target board:** DFRobot FireBeetle 2 ESP32-C6 (`dfrobot_firebeetle2_esp32c6`)

**Reference project:** the sibling `../DFR-MoistureTracker` established the
ESP-IDF/PlatformIO layout, native `esp_zb_*` Zigbee bring-up, OTA + partition/rollback
scheme, and z2m converter pattern. Follow its conventions; this project strips out the
battery/ADC/display/deep-sleep parts and swaps the end-device role for a router.

**Design spec:** [docs/superpowers/specs/2026-07-01-door-sensor-design.md](docs/superpowers/specs/2026-07-01-door-sensor-design.md)

## Key Differences from MoistureTracker

- **Zigbee Router**, not a sleepy end-device: `CONFIG_ZB_ZCZR=y` (not `CONFIG_ZB_ZED`),
  and **no** `CONFIG_PM_ENABLE` / tickless-idle / light-sleep. The radio stays on.
- **No deep sleep** — `app_main()` starts the stack and returns; the stack task runs
  forever. Reporting is event-driven off the door GPIO, not a wake-timer.
- **No ADC, battery monitor, soil sensor, or e-paper display.**
- Contact rides **IAS Zone (0x0500)**, recognized natively by z2m as a `contact` sensor.

## Build & Flash Commands

```bash
# Router build (deployment)
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor

# Bench build (identical; USB-JTAG stays up for observing join / re-flash)
pio run -e dfrobot_firebeetle2_esp32c6_zigbee_test -t upload -t monitor

# Host tests
pio test -e native

# Clean / erase flash (before re-provisioning in development)
pio run --target clean
pio run --target erase
```

## Architecture

`app_main()` runs once and never sleeps:

1. `init_system()` — NVS flash, event loop, door GPIO + ISR + debounce timer.
2. `door_contact_init()` — input GPIO with internal pull-up, install ISR, arm debounce.
3. `zb_router_init()` — start Zigbee as a Router; restore network state if joined,
   else begin BDB steering. Registers the endpoint + clusters.
4. Return. Stack task runs forever. GPIO edge → ISR → debounce → settled state →
   `ias_zone` attribute update → `ZoneStatusChangeNotification` to z2m.

### Module Responsibilities

| Module | File | Purpose |
|---|---|---|
| `door_contact` | `src/door_contact.c` | GPIO ISR + debounce one-shot timer; debounced state + change callback |
| `zb_router` | `src/zb_router.c` | Zigbee init as `ESP_ZB_DEVICE_TYPE_ROUTER`, join/steering, signal handler, endpoint/cluster registration |
| `ias_zone` | `src/ias_zone.c` | IAS Zone cluster: CIE write, enrollment, ZoneType 0x0015, ZoneStatus update/report |
| `ota_client` | `src/ota_client.c` | Zigbee OTA download + rollback self-check (ported from MoistureTracker) |
| `device_config` | `src/device_config.c` | NVS device name/id; Basic LocationDescription (0x0010) → z2m `label` |
| `main` | `src/main.c` | Wiring: init order, config constants, callback registration |

Modules marked as extraction candidates in the spec (`zb_router`, `ias_zone`,
`ota_client`, `device_config`) must keep **narrow, role-agnostic interfaces** — they
are intended to move into a shared `esp-zb-common` library in a later, separate spec.
Do not build that cross-repo library in this project.

### Key Configuration Constants (`src/main.c`)

```c
#define DEFAULT_DEVICE_ID        "door01"  // Compile-time name; rename in z2m UI
#define DOOR_CONTACT_GPIO        5         // Confirm against board pinout
#define DOOR_DEBOUNCE_MS         40        // Snap-action debounce window
#define KEEPALIVE_REPORT_MIN     60        // Periodic ZoneStatus refresh
#define DOOR_CONTACT_ACTIVE_LOW  1         // Polarity for NO vs NC wiring
```

## Hardware / Wiring

- KW11-2N SPDT snap-action switch (used only for logic-level sensing):
  **Common → input GPIO** (internal pull-up), **one throw → GND**, spare throw unused.
- Input pin **GPIO5** — avoid strapping pins (GPIO8/9/15) and the USB-Serial-JTAG pins.
- Debounce: both-edge ISR restarts a ~40 ms one-shot; sample settled level on expiry,
  report only on an actual state change.

## Flash Partitions

Reuse MoistureTracker's **dual-OTA layout** (`nvs`, `phy_init`, `otadata`, `ota_0`,
`ota_1`, `storage`, `zb_storage` fat, `zb_fct` fat) to support Zigbee OTA with
bootloader rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`).

## Naming Conventions

- Functions: `module_verb_noun()` — e.g. `door_contact_get_state()`
- Constants: `UPPER_SNAKE_CASE`. Types: `snake_case_t`.
- Log tags: short uppercase, e.g. `static const char *TAG = "DOOR";`

## Testing

- **Host (`pio test -e native`, Unity):** debounce state machine and IAS ZoneStatus
  bit encoding, as pure host-testable modules (mirrors MoistureTracker's `zigbee_encode`
  / `form_parser` tests).
- **On-device (manual):** join as router, confirm z2m shows a `contact` sensor and
  enrolled IAS Zone, actuate the switch for instant updates, confirm 60-min keepalive
  and that it relays for a downstream device.
