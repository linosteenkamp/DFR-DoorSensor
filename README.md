# DFR-DoorSensor

**ESP32-C6 mains/USB-powered Zigbee-router door/contact sensor**

A DFRobot FireBeetle 2 ESP32-C6 that reports a door's open/closed state to
zigbee2mqtt as a standard **IAS Zone contact sensor** (cluster 0x0500). Because it
joins the mesh as a **Zigbee Router** — not a sleepy end-device — it stays awake,
relays traffic for other devices, and extends network range. Reporting is
event-driven off a debounced GPIO, and firmware updates are delivered over the air
through zigbee2mqtt.

## Features

- **Contact sensing** — KW11-2N snap-action microswitch on a GPIO, both-edge ISR
  with a debounce one-shot timer; reports only on a settled state change
- **Zigbee Router** — always-on radio, relays traffic and extends the mesh
  (`CONFIG_ZB_ZCZR=y`); no deep sleep, no light sleep
- **IAS Zone contact** — rides the standard IAS Zone cluster (0x0500, ZoneType
  0x0015); the open/closed bit is a `ZoneStatusChangeNotification`, so updates are
  instant, plus a 60-minute keepalive re-assert for state recovery
- **Zigbee OTA** — CI-built images delivered through zigbee2mqtt with dual-slot
  bootloader rollback; fast (router, not a polled sleepy device)
- **Host-testable core** — the debounce state machine, contact mapping, and IAS
  ZoneStatus encoding are pure-C modules with a `native` unit-test suite

## How it works

`app_main()` runs once and never sleeps:

1. `init_system()` — NVS flash + default event loop.
2. `door_contact_init()` — configure the input GPIO (internal pull-up), install a
   both-edge ISR, arm the debounce timer.
3. `zb_router_init()` — start the Zigbee stack as a Router; restore network state if
   already joined, else run BDB steering. Registers the endpoint + clusters.
4. Cache the boot contact state and confirm a freshly-OTA'd image (rollback commit).
5. Return. The Zigbee stack task and `esp_timer` drive everything from here.

A door edge → ISR → debounce one-shot → settled level → `on_door_change()` →
`zb_router_set_contact()` → IAS Zone ZoneStatus update + notification to z2m.

## Hardware

- **Board**: DFRobot FireBeetle 2 ESP32-C6 (`dfrobot_firebeetle2_esp32c6`)
- **Contact input**: **GPIO5** (internal pull-up), clear of the strapping pins
  (GPIO8/9/15) and USB-JTAG
- **Switch**: KW11-2N SPDT snap-action, used only for logic-level sensing

```
FireBeetle 2 ESP32-C6              KW11-2N (SPDT)
┌─────────────────────┐          ┌───────────┐
│  GPIO 5 (pull-up) ───┼──────────┤ C (common)│
│  GND             ────┼──────────┤ NO / NC   │  one throw → GND
└─────────────────────┘          └───────────┘   spare throw unused
```

Wiring, GPIO choice, debounce, and polarity are covered in
[HARDWARE.md](HARDWARE.md). Powered from USB-C or any 5 V/3.3 V mains supply — there
is no battery and no low-power mode.

## Getting Started

### 1. Build & flash

```bash
# Router build (deployment)
pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t upload -t monitor

# Bench build (identical firmware; a named env kept for symmetry / future tweaks)
pio run -e dfrobot_firebeetle2_esp32c6_zigbee_test -t upload -t monitor

# Host unit tests
pio test -e native
```

### 2. Pair with zigbee2mqtt

1. Install the converter: copy `z2m/dfr_door_sensor.js` into the zigbee2mqtt config
   dir and register it under `external_converters:` in `configuration.yaml`, then
   restart z2m. Without it z2m shows the device as *"Not supported"* (z2m matches on
   manufacturer + model, and this DIY model has no built-in definition).
2. Enable `permit_join` in zigbee2mqtt.
3. Flash + power the device; it auto-runs BDB steering and joins as a Router.
4. Trigger a **Reconfigure / Re-interview** in the z2m UI so `configure()` writes the
   IAS CIE address and enrolls the zone — required before the device will send
   ZoneStatus notifications.
5. The device appears as `DFR-DoorSensor` (vendor `DFRobot-DIY`) exposing a `contact`
   entity. Rename it via `friendly_name` in the z2m UI.

Full pairing + OTA setup is in [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md).

## Reporting

The sensor exposes a single **`contact`** entity via the IAS Zone cluster:

| Layer | Value |
|-------|-------|
| Cluster | IAS Zone `0x0500`, ZoneType `0x0015` (contact switch) |
| Attribute | ZoneStatus `0x0002`, `alarm_1` bit = **door open** |
| z2m expose | `contact` — `true` = closed, `false` = open (the converter inverts `alarm_1`) |
| Cadence | instant on change (ZoneStatusChangeNotification) + 60-min keepalive re-assert |

## OTA Updates

Firmware updates are delivered over the air through zigbee2mqtt. A GitHub Actions
workflow (`.github/workflows/release-ota.yml`) builds the firmware on a `v*` tag,
wraps it into a `.ota` image, attaches it to a GitHub Release, and updates
`ota/index.json`; z2m reads that index over HTTPS and pushes the image to the
device, which writes it to the inactive OTA slot, reboots, and self-validates (with
bootloader rollback if the new image fails to boot).

Because this is a **mains-powered router**, the coordinator pushes blocks at airtime
speed — an update takes **minutes, not the multi-hour transfer a sleepy end-device
sees**. Full procedure (cutting a release, staged rollout, rollback, the version
encoding) is in [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md).

## Configuration

Tunables live in [src/main.c](src/main.c):

```c
#define DEFAULT_DEVICE_ID        "door01"  // Basic-cluster model/name; rename in z2m UI
#define DOOR_CONTACT_GPIO        5         // Contact input pin
#define DOOR_DEBOUNCE_MS         40        // Snap-action debounce window
#define DOOR_CONTACT_ACTIVE_LOW  1         // Polarity: 1 = LOW level is "open"
#define JOIN_TIMEOUT_MS          30000     // Fast-path join wait (stack keeps retrying after)
```

The keepalive interval (`KEEPALIVE_REPORT_MIN`, 60 min) is in
[src/zb_router.c](src/zb_router.c); the OTA query interval
(`OTA_QUERY_INTERVAL_MIN`, 30 min) is in [src/ota_client.c](src/ota_client.c).

## Architecture

Modular C with host-testable cores. Key modules:

| Module | File | Purpose |
|--------|------|---------|
| `contact_map` | `src/contact_map.c` | Map a raw GPIO level + polarity → logical door-open (pure) |
| `debounce` | `src/debounce.c` | One-shot debounce state machine; settle → state change (pure) |
| `door_contact` | `src/door_contact.c` | GPIO input + both-edge ISR + debounce timer; debounced state + callback |
| `ias_status` | `src/ias_status.c` | Encode logical door state into the IAS ZoneStatus bitmap (pure) |
| `zb_router` | `src/zb_router.c` | Zigbee Router init, join/steering, signal handler, endpoint/cluster registration, IAS Zone reporting, keepalive |
| `ota_client` | `src/ota_client.c` | Zigbee OTA Upgrade client + rollback self-check |
| `main` | `src/main.c` | Boot orchestration, config constants, callback wiring |

See [CLAUDE.md](CLAUDE.md) for the module contract and extraction plan.

## Documentation

- **[DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md)** — maintenance/extension, Zigbee router build, OTA procedure
- **[HARDWARE.md](HARDWARE.md)** — KW11-2N switch wiring, GPIO choice, debounce, polarity, troubleshooting
- **[PARTITIONS.md](PARTITIONS.md)** — flash partition layout (dual-OTA) and NVS
- **[CLAUDE.md](CLAUDE.md)** — architecture overview + module responsibilities
- **[docs/ota-api-notes.md](docs/ota-api-notes.md)** — esp-zigbee-lib 1.6.x OTA client API reference

## Quick Reference

### Pin assignments
| Function | GPIO | Notes |
|----------|------|-------|
| Door contact input | GPIO 5 | Internal pull-up; KW11-2N common. Clear of strapping/USB-JTAG pins |
| GND | GND | KW11-2N throw |

### Build environments
| Env | Role | Notes |
|-----|------|-------|
| `dfrobot_firebeetle2_esp32c6_zigbee` | Zigbee Router | production build |
| `dfrobot_firebeetle2_esp32c6_zigbee_test` | Zigbee Router | bench (identical; named env for symmetry) |
| `native` | — | host unit tests |

### Defaults
| Setting | Value |
|---------|-------|
| Default device ID (Basic model/name) | `door01` |
| Contact GPIO | 5 |
| Debounce window | 40 ms |
| Contact polarity | active-low (`DOOR_CONTACT_ACTIVE_LOW=1`) |
| Keepalive report interval | 60 min |
| OTA query interval | 30 min |
| OTA identity | manufacturerCode `0xFEFE`, imageType `0x0002` |

## Troubleshooting

- **z2m shows "Not supported"** — the converter isn't loaded; install
  `z2m/dfr_door_sensor.js` and restart z2m (see [Getting Started](#2-pair-with-zigbee2mqtt)).
- **`contact` never changes / stuck** — the IAS zone isn't enrolled; trigger a
  Reconfigure/Re-interview so `configure()` writes the CIE address.
- **Open/closed inverted** — flip `DOOR_CONTACT_ACTIVE_LOW` in `src/main.c` (NO vs NC
  wiring) — see [HARDWARE.md](HARDWARE.md).
- **Won't pair** — enable `permit_join`; erase + re-flash
  (`pio run -e dfrobot_firebeetle2_esp32c6_zigbee -t erase -t upload`) to clear stale
  network credentials.
- **Won't OTA-update** — see [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md); confirm the z2m
  `ota` override index points at this repo's `ota/index.json`.

## License

Provided as-is for educational and development purposes.
