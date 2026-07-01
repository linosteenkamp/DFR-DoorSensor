# DFR-DoorSensor — Design Spec

**Date:** 2026-07-01
**Status:** Approved (design), pending implementation plan
**Target board:** DFRobot FireBeetle 2 ESP32-C6 (`dfrobot_firebeetle2_esp32c6`)
**Reference project:** `../DFR-MoistureTracker` (established ESP-IDF/PlatformIO + native Zigbee patterns)

## 1. Overview & Scope

A mains/USB-powered **Zigbee Router** that senses a door via a KW11-2N snap-action
microswitch and reports open/closed to Zigbee2MQTT as a standard **IAS Zone contact
sensor**. Because it joins as a router (not a sleepy end-device), it also relays
traffic and extends the existing mesh (50+ devices in service).

No battery monitoring, no deep sleep, no ADC, no display.

### In scope (v1)
- Zigbee **Router** bring-up on the native 802.15.4 radio (radio always on, no sleep).
- **IAS Zone (0x0500)** contact reporting: GPIO interrupt + debounce, instant report
  on every state change, plus a periodic keepalive refresh.
- **Zigbee OTA** (cluster 0x0019) with dual-OTA partitions and bootloader rollback,
  ported from MoistureTracker.
- Clean module seams so the reusable core can later be extracted into a shared
  `esp-zb-common` library (its own future spec).

### Out of scope (YAGNI for v1)
- Config portal (SoftAP). Device name is **compile-time**; rename in the z2m UI.
- Status LED, e-paper display, battery monitoring.
- Tamper detection via the spare SPDT throw.

## 2. Reuse Strategy — Design for Extraction

Between MoistureTracker (sleepy end-device) and this door sensor (always-on router)
the genuinely reusable overlap is: Zigbee stack bring-up, OTA client + partition /
rollback layout, NVS device-naming, and the z2m converter pattern. The two devices
diverge on the hardest-to-abstract part — role and sleep policy — so the abstraction
is **not yet proven**.

Decision: **design-for-extraction, do not extract now.** Build the door sensor
standalone with deliberate module seams. A later, separate spec lifts the reusable
modules into a shared `esp-zb-common` library (PlatformIO `lib_deps` git dependency
or git submodule) once this project validates the interface against a second, different
device type. Do not build cross-repo library infrastructure in this project.

## 3. Architecture

`app_main()` runs once and never returns to sleep:

1. `init_system()` — NVS flash init, event loop, door GPIO + ISR + debounce timer.
2. `door_contact_init()` — configure the input GPIO with internal pull-up, install
   the ISR, arm the debounce timer machinery.
3. `zb_router_init()` — start the Zigbee stack as a **Router**; restore persisted
   network state if already joined, otherwise begin BDB steering (auto-join into any
   network with permit-join open). Registers the endpoint and clusters.
4. Return. The Zigbee stack task runs forever. A GPIO edge → ISR → debounce timer →
   settled state → `ias_zone` attribute update → Zigbee `ZoneStatusChangeNotification`.

### Data flow

```
KW11-2N ──▶ GPIO (pull-up) ──▶ ISR (both edges) ──▶ restart debounce one-shot (~40 ms)
                                                          │ on expiry: sample settled level
                                                          ▼
                                        door_contact state-change callback
                                                          ▼
                          ias_zone_set_status(open/closed)  ──▶  ZoneStatus attr (bit0=alarm1)
                                                          ▼
                              Zigbee stack  ──▶  ZoneStatusChangeNotification ──▶ z2m
```

## 4. Modules

`⇢ core` = extraction candidate for the future `esp-zb-common` library. Each module
gets a header exposing a narrow interface so the `⇢` modules can move to a library
later without touching call sites.

| Module | File | Purpose | Extraction |
|---|---|---|---|
| `door_contact` | `src/door_contact.c` | GPIO ISR + debounce one-shot timer; exposes debounced open/closed state and a state-change callback. Device-specific. | stays local |
| `zb_router` | `src/zb_router.c` | Zigbee init as `ESP_ZB_DEVICE_TYPE_ROUTER`, BDB steering / join, signal handler, endpoint + cluster registration. **No** light-sleep arming (router stays awake). | ⇢ core (parameterize role + endpoint builder) |
| `ias_zone` | `src/ias_zone.c` | IAS Zone cluster: CIE address write handling, zone enrollment, ZoneType `0x0015` (contact switch), ZoneStatus update + report helper. | ⇢ core (generic IAS Zone helper) |
| `ota_client` | `src/ota_client.c` | Zigbee OTA download + rollback self-check gate (ported from MoistureTracker). | ⇢ core |
| `device_config` | `src/device_config.c` | NVS device name/id; Basic-cluster LocationDescription (0x0010) → z2m `label`. | ⇢ core |
| `main` | `src/main.c` | Wiring: init order, config constants, callback registration. | stays local |

## 5. Zigbee Behavior (IAS Zone)

- **Endpoint:** single endpoint, Home Automation profile, IAS Zone sensor device id.
- **Clusters:** Basic (0x0000, incl. LocationDescription for the z2m label),
  Identify (0x0003), IAS Zone (0x0500, `ZoneType = 0x0015` contact switch),
  OTA Upgrade (0x0019).
- **Join:** BDB steering into any permit-join network — same pattern as MoistureTracker
  minus the light-sleep arming. Router role means the device keeps the radio on and
  relays for neighbours.
- **Enrollment:** respond to the coordinator's CIE address write and the zone-enroll
  request so z2m marks the zone enrolled.
- **Reporting:** **instant** `ZoneStatusChangeNotification` on every debounced edge,
  plus a periodic status refresh (default **60 min**) as a keepalive / state-recovery
  safety net.
- **ZoneStatus:** bit 0 (alarm1) = contact open. Contact polarity (which switch throw
  counts as "closed") is set by compile-time flag `DOOR_CONTACT_ACTIVE_LOW`, allowing
  either NO or NC wiring.
- **z2m:** IAS Zone contact sensors are recognized natively — expected to need **no
  custom converter**. If the `label` field is wanted, a small converter mirroring
  MoistureTracker's `z2m/` pattern exposes it. Verified against the live z2m during
  bring-up.

## 6. Hardware / Wiring

- **KW11-2N** (SPDT snap-action, 5 A contacts — only used for logic-level sensing):
  - **Common → input GPIO** (internal pull-up enabled).
  - **One throw (NC or NO) → GND.** Spare throw left unconnected (reserved for a
    future tamper input).
- **Input pin:** **GPIO5** (noted free in MoistureTracker's pinout), avoiding strapping
  pins (GPIO8 / GPIO9 / GPIO15) and the USB-Serial-JTAG pins. Final pin confirmed
  against the board pinout during implementation.
- **Debounce:** ISR on both edges restarts a ~30–50 ms one-shot timer (default 40 ms);
  on expiry the settled level is sampled and a change is reported only if the debounced
  state actually differs. Handles snap-action chatter.

## 7. Build, Partitions, Config

- **PlatformIO / ESP-IDF**, mirroring MoistureTracker's env structure:
  - `dfrobot_firebeetle2_esp32c6_zigbee` — router build (deployment).
  - `dfrobot_firebeetle2_esp32c6_zigbee_test` — bench build (identical; used for
    observing join / re-flashing over USB-JTAG).
  - `native` — host test env (Unity).
- **`sdkconfig.defaults.zigbee`:** `CONFIG_ZB_ENABLED=y`, **`CONFIG_ZB_ZCZR=y`**
  (Zigbee Coordinator/Router library — router role, **not** `CONFIG_ZB_ZED`),
  `CONFIG_ZB_RADIO_NATIVE=y`, `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`, custom partition
  table, `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. **No** `CONFIG_PM_ENABLE` /
  `CONFIG_FREERTOS_USE_TICKLESS_IDLE` / `CONFIG_ESP_PHY_MAC_BB_PD` — a router must
  stay awake.
- **Partitions:** reuse MoistureTracker's dual-OTA layout (`nvs`, `phy_init`,
  `otadata`, `ota_0`, `ota_1`, `storage`, `zb_storage` fat, `zb_fct` fat) to support
  Zigbee OTA with rollback.
- **Key constants (`src/main.c`):**
  - `DEFAULT_DEVICE_ID "door01"` — compile-time device name (rename in z2m).
  - `DOOR_CONTACT_GPIO 5`
  - `DOOR_DEBOUNCE_MS 40`
  - `KEEPALIVE_REPORT_MIN 60`
  - `DOOR_CONTACT_ACTIVE_LOW 1` — polarity for NO vs NC wiring.

## 8. Testing

- **Host (`native`, Unity):** pure-logic modules included directly in tests, following
  MoistureTracker's host-test pattern:
  - **Debounce state machine** — edge sequences produce the correct settled state and
    fire the change callback exactly once per real transition.
  - **IAS ZoneStatus bit encoding** — open/closed + polarity flag map to the correct
    ZoneStatus bits.
- **On-device (manual):** flash the router build; confirm it joins the mesh as a
  router; confirm z2m exposes it as a `contact` sensor and marks IAS Zone enrolled;
  actuate the switch and confirm instant open/closed updates; confirm the 60-min
  keepalive; confirm it relays for at least one downstream device.

## 9. Naming Conventions (inherited from MoistureTracker)

- Functions: `module_verb_noun()` — e.g. `door_contact_get_state()`.
- Constants: `UPPER_SNAKE_CASE`. Types: `snake_case_t`.
- Log tags: short uppercase, e.g. `static const char *TAG = "DOOR";`.

## 10. Open Questions / Deferred

- Exact free GPIO confirmed against the FireBeetle 2 ESP32-C6 pinout at implementation.
- Whether z2m needs any converter at all for the `label` field (verify on live z2m).
- Future `esp-zb-common` extraction — separate spec once a second device type ships.
