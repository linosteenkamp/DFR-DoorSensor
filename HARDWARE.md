# Hardware & Wiring — KW11-2N Door Contact

## Switch

The sensor uses a **KW11-2N SPDT snap-action microswitch** (a common lever
microswitch) purely for logic-level sensing — no current is switched, only a GPIO
level. It has three terminals:

- **C** — common
- **NO** — normally-open throw
- **NC** — normally-closed throw

Only the common and **one** throw are used; the spare throw is left unconnected. A
magnet is not involved — the door's motion actuates the lever (or a bracket the door
presses).

## Wiring

```
FireBeetle 2 ESP32-C6              KW11-2N (SPDT)
┌─────────────────────┐          ┌───────────┐
│  GPIO 5 (pull-up) ───┼──────────┤ C (common)│
│  GND             ────┼──────────┤ NO or NC  │  one throw → GND
└─────────────────────┘          └───────────┘   spare throw unused
```

- **Common → GPIO5**, which is configured as an input with the ESP32-C6's **internal
  pull-up**. So the pin idles HIGH and reads LOW when the chosen throw shorts it to
  GND.
- **One throw → GND.** Whether you use NO or NC decides the resting level; the
  firmware's polarity setting adapts to either (see [Polarity](#polarity)).
- **Spare throw unused.**

No external resistors are needed — the internal pull-up is sufficient for a
mains-powered device with the switch close to the board.

## Why GPIO5

GPIO5 is the confirmed contact pin. It is:

- clear of the ESP32-C6 **strapping pins** (GPIO8, GPIO9, GPIO15), which must sit at
  defined levels at boot;
- clear of the **USB-Serial-JTAG** pins, so flashing/monitoring is unaffected;
- a plain digital input with internal pull-up available.

To move the pin, change `DOOR_CONTACT_GPIO` in [src/main.c](src/main.c) and keep the
same constraints in mind.

## Debounce

Snap-action switches bounce for a few milliseconds on each transition. The firmware
debounces in hardware-interrupt + timer form:

1. A **both-edge ISR** fires on every level change.
2. The ISR **restarts a one-shot timer** (`DOOR_DEBOUNCE_MS`, default **40 ms**).
3. When the timer finally expires (no edge for the full window), the level is
   **sampled** and compared to the last stable level.
4. A change is reported **only** when the settled level actually differs — so bounce
   never produces spurious reports.

The debounce logic (`debounce.c`) and the raw-level→open mapping (`contact_map.c`)
are pure C and covered by the `native` unit tests (`test_debounce`,
`test_contact_map`).

## Polarity

`DOOR_CONTACT_ACTIVE_LOW` in [src/main.c](src/main.c) selects how the pin level maps
to logical **door-open**:

| `DOOR_CONTACT_ACTIVE_LOW` | Pin LOW means | Pin HIGH means |
|---|---|---|
| `1` (default) | door **open** | door **closed** |
| `0` | door **closed** | door **open** |

With the internal pull-up, the pin is LOW only while the chosen throw shorts it to
GND, so the right value depends on whether you wired **NO** or **NC** and on the
switch's resting position relative to the door. If z2m reports open/closed inverted,
flip this constant, rebuild, and re-flash — no rewiring required.

> The default `1` was confirmed on the bench unit (corrected from `0` in v1.0.1).

## Power

The device is **mains/USB powered** and runs as a Zigbee Router — the radio is always
on so it can relay for other nodes. There is no battery, no ADC/battery monitor, and
no sleep mode. Power it from USB-C or any stable 3.3 V/5 V supply the FireBeetle 2
accepts.

## Troubleshooting

- **`contact` never changes** — the IAS zone likely isn't enrolled; trigger a
  Reconfigure/Re-interview in z2m (writes the CIE address). See
  [DEVELOPER_GUIDE.md](DEVELOPER_GUIDE.md).
- **Open/closed inverted** — flip `DOOR_CONTACT_ACTIVE_LOW` in `src/main.c`.
- **Chattery / double reports** — increase `DOOR_DEBOUNCE_MS`; check the switch is
  mechanically seating cleanly and the common/throw wiring is solid.
- **No reaction at all** — confirm the throw is on **GND** and the common on
  **GPIO5**; watch the serial monitor for the door-configured log line and for edge
  activity when you actuate the switch.
- **Flashing fails after wiring** — make sure nothing was moved onto a strapping or
  USB-JTAG pin; GPIO5 avoids these.
