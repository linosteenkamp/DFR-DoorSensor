# Developer Documentation — DFR-DoorSensor

## Overview

Technical reference for developers maintaining or extending the DFR-DoorSensor
firmware. The device is a mains/USB-powered ESP32-C6 that joins Zigbee as a
**Router** and reports a debounced door contact to zigbee2mqtt over the standard
**IAS Zone** cluster, with over-the-air firmware updates.

This project descends from the sibling `../DFR-MoistureTracker`, which established
the ESP-IDF/PlatformIO layout, the native `esp_zb_*` Zigbee bring-up, and the OTA +
partition/rollback scheme. DoorSensor strips out the battery/ADC/display/deep-sleep
parts and swaps the sleepy end-device role for an always-on router.

## Project Structure

```
DFR-DoorSensor/
├── include/               # Public headers
│   ├── contact_map.h      # Raw GPIO level → logical door-open (pure)
│   ├── debounce.h         # Debounce one-shot state machine (pure)
│   ├── door_contact.h     # GPIO + ISR + debounce timer; debounced state + callback
│   ├── ias_status.h       # IAS Zone ZoneStatus bit encoding (pure)
│   ├── zb_router.h        # Zigbee Router init / join / IAS Zone reporting
│   ├── ota_client.h       # Zigbee OTA Upgrade client + rollback self-check
│   ├── ota_ids.h          # OTA identity (manuf/imageType/model) + version packing
│   └── fw_version.h       # FW_VER_* (build-injected) → FW_VERSION_U32 / _STR
├── src/                   # Implementations (+ CMakeLists.txt, idf_component.yml)
│   ├── contact_map.c
│   ├── debounce.c
│   ├── door_contact.c
│   ├── ias_status.c
│   ├── zb_router.c
│   ├── ota_client.c
│   └── main.c             # Boot orchestration
├── test/                  # Unity host tests (env: native)
│   ├── test_smoke/  test_contact_map/  test_debounce/
│   ├── test_ias_status/  test_ota_version/
├── tools/                 # OTA image + index tooling (+ pytest)
│   ├── make_ota_image.py  update_ota_index.py  test_ota_tools.py
├── z2m/                   # zigbee2mqtt external converter + icon asset
│   ├── dfr_door_sensor.js  dfr_door_sensor.svg
├── ota/index.json         # OTA discovery index (committed by CI on release)
├── partitions.csv         # Flash layout (dual-OTA) — see PARTITIONS.md
├── platformio.ini         # PlatformIO project configuration
└── *.md                   # Documentation
```

## Architecture

### Design principles

- **Single responsibility** — each module has one clear purpose.
- **Host-testable cores** — decision logic (contact mapping, debounce settling,
  ZoneStatus encoding) is pure C with no ESP-IDF dependency, so it runs under the
  `native` Unity suite. The ESP-specific glue (`door_contact`, `zb_router`,
  `ota_client`) is thin.
- **Narrow, role-agnostic interfaces** — `zb_router`, `ias_status`, and `ota_client`
  keep interfaces that could move into a shared `esp-zb-common` library later. Do
  **not** build that cross-repo library in this project.

### Module dependency graph

```
                          main.c
                             |
        ┌────────────────────┼────────────────────┐
        |                    |                     |
   door_contact          zb_router            ota_client
        |                    |                     |
   ┌────┴────┐          ias_status          (esp-zigbee OTA
   |         |          (ZoneStatus)          cluster 0x0019)
contact_map debounce         |
 (pure)     (pure)      (esp-zigbee stack)
```

`main.c` wires them: a door edge handled by `door_contact` (via `contact_map` +
`debounce`) invokes the change callback, which calls `zb_router_set_contact()`,
which encodes state with `ias_status` and pushes an IAS Zone report.

### Key subsystems

#### 1. Contact pipeline (`contact_map.c`, `debounce.c`, `door_contact.c`)

**Purpose**: turn noisy switch edges into settled logical open/closed events.

- **`contact_map`** — `contact_is_open(gpio_level, active_low)`: pure mapping of a raw
  pin level to logical door-open, honoring NO vs NC polarity.
- **`debounce`** — a one-shot state machine. The both-edge ISR restarts a ~40 ms
  timer on every edge; on expiry the level is sampled and `debounce_settle()` reports
  a change only when the settled level differs from the last stable one.
- **`door_contact`** — the ESP glue: configures the input GPIO with internal pull-up,
  installs the ISR, arms the `esp_timer` one-shot, and invokes the registered
  `door_contact_cb_t` on a settled change. Exposes `door_contact_is_open()`.

**Extension points**:
- Change the debounce window: `DOOR_DEBOUNCE_MS` in `src/main.c`.
- Change the pin: `DOOR_CONTACT_GPIO` (keep clear of strapping pins GPIO8/9/15 and
  USB-JTAG).
- Multiple contacts: replicate the `door_contact` instance state and map each to its
  own IAS Zone endpoint.

#### 2. Zigbee Router (`zb_router.c`)

**Purpose**: bring up the stack as a Router, own the endpoint/clusters, and report
IAS Zone status.

**Key points**:
- Role is `ESP_ZB_DEVICE_TYPE_ROUTER` (`CONFIG_ZB_ZCZR=y`). The radio is always on;
  there is **no** `CONFIG_PM_ENABLE`, tickless idle, or light sleep.
- Registers the Basic cluster identity (manufacturer `DFRobot-DIY`, model
  `DFR-DoorSensor`) so z2m can match the converter.
- Registers the IAS Zone server cluster (0x0500, ZoneType 0x0015), handles the CIE
  write + enrollment, and reports ZoneStatus.
- `zb_router_set_contact(open)` acquires the Zigbee lock, encodes the state via
  `ias_status`, updates the ZoneStatus attribute, and emits a
  ZoneStatusChangeNotification.
- A `keepalive_cb` scheduler alarm re-asserts the current ZoneStatus every
  `KEEPALIVE_REPORT_MIN` (60 min) for state recovery if a notification was missed.

#### 3. IAS ZoneStatus encoding (`ias_status.c`)

`ias_zone_status_encode(open)` → `0x0001` (alarm_1 set) when open, `0x0000` when
closed. Pure and host-tested (`test_ias_status`).

#### 4. OTA client (`ota_client.c`)

Adds the Zigbee OTA Upgrade client cluster (0x0019) and the rollback self-check.
Advertises `manufacturerCode 0xFEFE`, `imageType 0x0002`, and a `fileVersion` packed
from the git tag via `OTA_PACK_VERSION` (`include/ota_ids.h`). See
[OTA Updates](#ota-updates) below and
[docs/ota-api-notes.md](docs/ota-api-notes.md) for the underlying esp-zigbee API.

## Adding new features

### Adding a second contact / endpoint

1. Add a header + `.c` following the `door_contact` pattern (own GPIO, ISR, timer,
   debounce state).
2. Register a second endpoint with its own IAS Zone cluster in `zb_router.c`.
3. Wire its callback in `main.c` to a `zb_router_set_contact_ep(ep, open)` variant.
4. Add the exposed contact to the z2m converter (`z2m/dfr_door_sensor.js`).

### Adding a host-testable decision module

Keep new logic pure where possible so it can be unit-tested on the host:

```c
// include/foo.h — no ESP-IDF includes
bool foo_decide(int input, bool flag);
```

```c
// test/test_foo/test_foo.c
#include <unity.h>
#include "../../src/foo.c"   // tests #include the SUT .c directly
```

Then add `test_foo` to the `test_filter` list under `[env:native]` in
`platformio.ini`. Tests link only the pure `.c`, so nothing pulls in ESP headers.

## Code style guidelines

### Naming conventions

- **Functions**: `module_verb_noun()` — e.g. `door_contact_get_state()`
- **Files**: `snake_case.c`
- **Constants**: `UPPER_SNAKE_CASE`. **Types**: `snake_case_t`.
- **Log tags**: short uppercase — `static const char *TAG = "DOOR";`

### Error handling

Check return values and log with the right level:

```c
esp_err_t err = some_function();
if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to do something: %s", esp_err_to_name(err));
    return err;
}
```

- `ESP_LOGE` — errors that prevent operation
- `ESP_LOGW` — non-critical warnings (e.g. join timeout; the stack keeps retrying)
- `ESP_LOGI` — milestones (boot, join, OTA confirmed)
- `ESP_LOGD` — verbose debug

## Testing

### Host tests (`pio test -e native`)

Unity tests cover the pure cores — the debounce state machine, contact mapping, IAS
ZoneStatus encoding, and the OTA version packing (incl. the exact z2m display
string). The OTA image/index tooling has its own pytest suite:

```bash
pio test -e native                       # Unity host tests
python -m pytest tools/test_ota_tools.py -v   # OTA tool tests
```

Tests `#include` the SUT `.c` directly and `[env:native]` excludes `src/` from the
regular build (`build_src_filter = -<*>`), so `pio run` never compiles ESP-IDF code
on the host.

### On-device manual checklist

- [ ] Joins zigbee2mqtt as a **Router** without interview errors.
- [ ] After installing the converter + Reconfigure/Re-interview, a `contact` entity
      appears and the IAS zone shows enrolled.
- [ ] Actuating the switch flips `contact` **instantly** (open → `contact: false`).
- [ ] Polarity is correct; if inverted, flip `DOOR_CONTACT_ACTIVE_LOW`.
- [ ] The 60-minute keepalive re-asserts ZoneStatus (state recovers after a missed
      notification).
- [ ] The device **relays** for a downstream device (confirms router behavior).

### Serial monitor validation

Expected boot sequence (abbreviated; ESP-IDF stack lines omitted):

```
I MAIN: DFR-DoorSensor boot (door01)
I DOOR: contact GPIO5 configured (pull-up, active_low=1)
I ZB_ROUTER: starting as ROUTER
I ZB_ROUTER: joined  (or)  I MAIN: joined on the fast path
I MAIN: OTA image confirmed valid          # only after a fresh OTA
```

## Useful commands

```bash
pio run -e dfrobot_firebeetle2_esp32c6_zigbee                 # build
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload       # flash
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor
pio run -t erase                                              # erase flash (re-pair)
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t size         # image size vs slot
pio run -t clean
pio test -e native                                            # host tests
```

## Zigbee Router build

The Zigbee env injects `sdkconfig.defaults.zigbee` via
`board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.zigbee"`
(the only mechanism that works for a pure-espidf build here). It enables the
802.15.4 radio, the **router** role (`CONFIG_ZB_ZCZR=y`), 4 MB flash, the
`zb_storage`/`zb_fct` partitions, and bootloader rollback
(`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`).

| Env | Role | Sleep | Use |
|-----|------|-------|-----|
| `dfrobot_firebeetle2_esp32c6_zigbee` | Router | none (radio always on) | deployment |
| `dfrobot_firebeetle2_esp32c6_zigbee_test` | Router | none | bench (identical; named env for symmetry) |

### Power model — always on

Unlike the MoistureTracker end-device, the DoorSensor does **not** sleep.
`app_main()` starts the stack and returns; the esp-zigbee task stays alive and the
radio is always receiving so it can relay for other nodes. Reporting is
**event-driven** off the door GPIO (plus the 60-min keepalive), not a wake timer.

### Pairing

1. Install `z2m/dfr_door_sensor.js` as an external converter and restart z2m.
2. Enable `permit_join`; power/flash the device — it auto-runs BDB steering and joins.
3. Trigger a **Reconfigure / Re-interview** so `configure()` writes the IAS CIE
   address and enrolls the zone (required before ZoneStatus notifications flow).
4. The device appears as `DFR-DoorSensor` (vendor `DFRobot-DIY`) with a `contact`
   entity. The modelID matches `zigbeeModel`, so a z2m restart re-applies the
   converter without re-pairing.

## OTA Updates

`src/ota_client.c` adds the Zigbee OTA Upgrade client cluster (0x0019). The device
advertises `manufacturerCode 0xFEFE`, `imageType 0x0002`, and a `fileVersion` packed
from the git tag. z2m serves images from a GitHub Releases asset; `ota/index.json` is
the discovery index z2m polls.

### One-time bootstrap (per node, over USB)

The OTA firmware uses the **dual-OTA partition table** (`otadata` + `ota_0` + `ota_1`,
1.5 MB each — see [PARTITIONS.md](PARTITIONS.md)). The first OTA-capable build must be
flashed over USB; after that all updates are wireless.

```bash
# Erase old table + flash OTA-capable firmware in one step (wipes Zigbee creds → re-pair):
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t erase -t upload
```

### z2m configuration

Add (or extend) the `ota:` block in zigbee2mqtt's `configuration.yaml`:

```yaml
ota:
  # Raw GitHub URL of ota/index.json on the default branch (master):
  zigbee_ota_override_index_location: "https://raw.githubusercontent.com/linosteenkamp/DFR-DoorSensor/master/ota/index.json"
  # Roll out manually, one device at a time:
  disable_automatic_update_check: true
```

The converter already declares `ota: true`, so no converter change is needed. The
repo is public, so z2m fetches both the index (`raw.githubusercontent.com`) and the
release `.ota` asset over anonymous HTTPS — no token or self-hosted server required.

### Cutting a release

Tag the commit you want to ship and push the (annotated, `v`-prefixed) tag:

```bash
git tag -a v1.2.3 -m "v1.2.3"
git push origin v1.2.3
```

`.github/workflows/release-ota.yml` then:
1. Derives `FW_VER_MAJOR/MINOR/PATCH` from the tag, injects them as `-D` flags
   (→ `FW_VERSION_U32`), and runs the host + native tests.
2. Wraps `firmware.bin` into `firmware-<semver>.ota` (`tools/make_ota_image.py`).
3. Publishes a GitHub Release with that asset attached.
4. Updates `ota/index.json` (`tools/update_ota_index.py`) and commits it to `main`.

The workflow triggers only on `tags: ['v*']` — a bare `1.2.3` tag creates no run.

### Version encoding — must display correctly in z2m

z2m renders the OTA `fileVersion` as its 8-character **hex string**, split
`AB.CC-DE.FF` (`fileVersion2String`:
`hex[0].hex[1].hex[2:4]-hex[4].hex[5].hex[6:8]`). To make the **major** the leading
digit, the semver lives in the **high** hex digits:

```
OTA_PACK_VERSION(major, minor, patch) = (major & 0xF)<<28 | (minor & 0xF)<<24 | (patch & 0xFF)<<16
  e.g. v1.0.3 → 0x10030000 → z2m shows "1.0.03-0.0.00"
```

The CI derives the same value with `printf '0x%X%X%02X0000'`. The firmware's
`OTA_PACK_VERSION` and the CI's `file_version` **must** produce the same number.

- **Constraints**: major & minor are each a single hex nibble (0–15); patch is one
  byte shown in hex (patch ≥ 10 renders `0a`…). The `-0.0.00` stack suffix is normal
  — every z2m device shows one.
- **Ordering**: a release must produce a strictly larger `fileVersion` than the image
  currently on the target, or z2m won't offer it. The scheme is monotonic and stays
  above every legacy `0x01xxxxxx` value, so old firmware is still offered the upgrade.
  Local dev builds default to `0x00000000`; any tagged release supersedes them.

> **History**: v1.0.2 shipped a broken packing (`0x01<MA><MI><PA>`) that z2m
> displayed as `0.1.01-0.0.02` — the major hidden in the minor slot. Fixed in
> **v1.0.3**; `test/test_ota_version` now asserts both the packed uint32 and the
> exact z2m display string.

### Staged rollout / canary

With `disable_automatic_update_check: true`, z2m won't push without operator action:

1. In the z2m UI, open **one** device → **OTA** tab → **Update**.
2. Watch progress on the serial monitor or in z2m state. Because this is a
   **router**, expect the transfer to complete in **minutes** (the coordinator pushes
   blocks at airtime speed — there is no sleepy-device indirect-poll gate).
3. The device reboots into the new slot, rejoins, and `app_main()` confirms the image
   valid (`OTA image confirmed valid`).
4. Verify `contact` still tracks the switch, then repeat for the next node.

### Rollback behavior

The bootloader runs with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. A freshly flashed
image stays `ESP_OTA_IMG_PENDING_VERIFY` until `ota_client_mark_valid()` is called —
which happens as soon as `app_main()` is reached, i.e. **"booted cleanly = good"**
(gated on boot success, not on Zigbee join). If the new image crashes before reaching
`app_main()`, the bootloader reverts to the previous slot on the next boot.

> There is no SoftAP recovery or secure-boot bypass. If both slots are bad, re-flash
> over USB.

### Limitations and notes

- **Slot size**: each OTA slot is `0x180000` = **1.5 MB**. Monitor image growth with
  `pio run -t size`; exceeding the slot aborts the OTA at the write stage.
- **Router OTA is fast**: minutes, not the ~3 h a sleepy end-device (MoistureTracker)
  takes — the coordinator isn't gated by an indirect-poll round-trip.
- **Repo must stay public**: z2m fetches the index and `.ota` over unauthenticated
  HTTPS; a private repo would 404 for z2m.
- **Query interval**: the client polls the OTA server every 30 minutes
  (`OTA_QUERY_INTERVAL_MIN`), so after a release it may take up to 30 min before z2m
  marks the device "update available".

## Resources

- [ESP-IDF Documentation (ESP32-C6)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/)
- [PlatformIO ESP32 Platform](https://docs.platformio.org/en/latest/platforms/espressif32.html)
- [DFRobot FireBeetle 2 ESP32-C6 Wiki](https://wiki.dfrobot.com/SKU_DFR0868_FireBeetle_2_Board_ESP32_C6)
- [Zigbee IAS Zone cluster (ZCL spec)](https://zigbeealliance.org/wp-content/uploads/2019/12/07-5123-06-zigbee-cluster-library-specification.pdf)

## Contributing

1. Follow existing structure and style.
2. Keep decision logic in pure, host-tested modules where possible.
3. Update the relevant `.md` files.
4. Run `pio test -e native` and the pytest suite before committing.
5. Keep the `zb_router` / `ias_status` / `ota_client` interfaces narrow and
   role-agnostic (future shared-library candidates).
