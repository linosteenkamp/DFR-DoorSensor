// zigbee2mqtt external converter for the DFR-DoorSensor DIY door/contact sensor
// (ESP32-C6 Zigbee ROUTER, esp-zigbee 1.6.x). Exposes:
//   - contact   (IAS Zone cluster 0x0500, ZoneType 0x0015 contact switch;
//                ZoneStatus alarm_1 bit = door open). z2m's `contact` is
//                true = closed, false = open — alarm_1 is inverted to match.
//
// Why a converter at all: z2m identifies devices by Basic-cluster manufacturer
// name + model ID. This DIY device reports manufacturer "DFRobot-DIY" / model
// "DFR-DoorSensor", which no built-in definition matches, so z2m shows it as
// "Not supported" until this definition is loaded. (IAS Zone is a standard
// cluster, but z2m still needs a definition to expose `contact` for an unknown
// model.)
//
// Install (z2m 2.x): copy this file into the zigbee2mqtt config dir and add it
// under `external_converters:` in configuration.yaml:
//     external_converters:
//       - dfr_door_sensor.js
// (older z2m) or drop it in the `external_converters/` folder (newer z2m), then
// restart zigbee2mqtt. The device's modelID "DFR-DoorSensor" matches zigbeeModel,
// so no re-pair is needed. After the restart, trigger a device **re-interview**
// (z2m UI → device → Reconfigure/Re-interview) so z2m runs configure() below,
// which writes the IAS CIE address and enrolls the zone — required before the
// device will send ZoneStatusChangeNotifications.
//
// OTA: in zigbee2mqtt configuration.yaml set:
//   ota:
//     zigbee_ota_override_index_location: https://raw.githubusercontent.com/linosteenkamp/DFR-DoorSensor/main/ota/index.json
//     disable_automatic_update_check: true   # staged/manual rollout
// The definition opts in via `ota: true`; z2m matches an image by manufacturerCode
// 0xFEFE + imageType 0x0002 (distinct from the soil sensor's 0x0001).

const m = require('zigbee-herdsman-converters/lib/modernExtend');

module.exports = [
    {
        zigbeeModel: ['DFR-DoorSensor'],
        model: 'DFR-DoorSensor',
        vendor: 'DFRobot-DIY',
        description: 'ESP32-C6 Zigbee-router door/contact sensor (DIY)',
        extend: [
            // IAS Zone contact: exposes `contact`. alarm_1 (our ZoneStatus bit0 =
            // open) is mapped to z2m's contact semantics (open -> contact false).
            m.iasZoneAlarm({
                zoneType: 'contact',
                zoneAttributes: ['alarm_1'],
            }),
        ],
        // Mains-powered router — opt into z2m's OTA subsystem (image type 0x0002).
        ota: true,
    },
];
