# DFR-DoorSensor

ESP32-C6 (DFRobot FireBeetle 2) mains/USB-powered **Zigbee Router** door sensor.
A KW11-2N snap-action switch is reported to Zigbee2MQTT as a native **IAS Zone
contact** sensor. Because it joins as a router, it also relays traffic and
extends the mesh. Supports Zigbee OTA with rollback.

See [CLAUDE.md](CLAUDE.md) for architecture and
[docs/superpowers/specs/archive/2026-07-01-door-sensor-design.md](docs/superpowers/specs/archive/2026-07-01-door-sensor-design.md)
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
