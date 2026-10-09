# Zigbee range extender

[![Compile](https://github.com/gaborgoncz/zigbee-range-extender/actions/workflows/compile.yml/badge.svg)](https://github.com/gaborgoncz/zigbee-range-extender/actions/workflows/compile.yml)

A Zigbee router built on the **ESP32-H2 Super Mini**, with a LiPo cell that keeps it running through a power cut. Plug it into a USB charger halfway between the coordinator and the devices that have a weak link, and it forwards their traffic.

It starts from the `Zigbee_Range_Extender` example of arduino-esp32 and fixes what that example leaves out.

> **Status:** the firmware compiles (arduino-esp32 3.3.11). It has **not been run on hardware yet**: pairing, the LED colours, the diagnostics, the battery reading and cutoff, and the link watchdog are untested.

## What is different from the Arduino example

| Arduino example | This firmware |
|---|---|
| Transmit power left at the stack's default | Set in the sketch, 20 dBm by default |
| No sign of what it is doing | LED shows searching, joined, or joined but alone |
| Keeps believing it is joined when the network is gone | Restarts after 30 minutes without a link to any router or the coordinator |
| Hangs stay hung | A hanging loop or Zigbee stack restarts the device |
| Shows up in Zigbee2MQTT as an unsupported device | Converter with a name and diagnostics |
| Nothing to see in Home Assistant | Children, router links, uptime and restart reason |
| No idea of the backup cell | Battery level and voltage, and it switches itself off before the cell is drained |

## What shows up in Home Assistant

| Entity | Kind | Notes |
|---|---|---|
| Children | diagnostic | End devices attached to this router. Above 0 means devices use it |
| Router links | diagnostic | Routers and the coordinator it has a live link to. 0 means it cannot forward anything |
| Uptime | diagnostic | Minutes since the last restart |
| Restart reason | diagnostic | `power_on`, `software`, `crash`, `task_watchdog`, `brownout`, `deep_sleep`, ... |
| Battery | sensor | %. Reads high while USB power is charging the cell |
| Voltage | sensor | mV |
| Linkquality | sensor | From Zigbee2MQTT |

They are sent 30 seconds after joining and then every 5 minutes.

A **brownout** restart reason points at the power supply: a weak charger or a long thin USB cable. **software** is the link watchdog, a leave request from the coordinator, or the BOOT button. **deep_sleep** means it had switched itself off on an empty cell.

## LED

| LED | Meaning |
|---|---|
| Blue, blinking | Not on a network, searching. Enable **Permit join** |
| Red, blinking | On the network, but no router or coordinator in range |
| Off | Joined and linked |
| White, blinking | Identify, started from Zigbee2MQTT |
| Red, fast | BOOT is held, leaves the network after 3 seconds |

The sketch uses the RGB LED on GPIO8. Set `LED_WHEN_JOINED` to `1` for a dim green light while everything is fine.

## Parts

- ESP32-H2 Super Mini
- USB power supply and cable
- Single LiPo cell (1S, 3.7 V nominal) **with protection circuit**, optional
- 2 × 200 kΩ resistor for battery sensing
- 2.4 GHz FPC patch antenna on a coax lead, optional
- Slide switch in the cell's positive lead, optional

## Wiring

| ESP32-H2 Super Mini | Connects to |
|---|---|
| BAT+ (pad on the underside) | Cell positive through the switch, top of R1 |
| BAT− (pad on the underside) | Cell negative, bottom of R2 |
| GPIO1 | Middle of the R1/R2 divider |

Notes:

- Without the cell and the divider the firmware still works: it reports 0 % and skips all battery logic.
- Other resistor values work as long as the two are equal. For unequal ones set `BATTERY_DIVIDER_RATIO`.
- A 100 nF capacitor from GPIO1 to GND steadies the reading with resistors this large.
- One cell only. Cells in parallel are fine, two in series will destroy the board.

## On battery

A router cannot sleep between messages, its radio has to listen all the time. So on the cell it runs at full power until the cell is nearly empty:

- Below `BATTERY_CUTOFF_MV` (3.3 V) on three readings in a row it switches the radio off and goes to deep sleep.
- Every 10 minutes it wakes up for a moment and looks at the cell. It starts again once the cell is above `BATTERY_RESUME_MV` (3.6 V), which it reaches soon after the USB power is back.
- The devices that were attached to it look for another router in the meantime.

While it sleeps its USB port is gone. To flash it then, hold **BOOT** while plugging in the cable.

## Flashing

Requires the Arduino IDE or `arduino-cli` with the **esp32** board package 3.3 or newer. No extra libraries.

Board settings:

| Setting | Value |
|---|---|
| Board | ESP32H2 Dev Module |
| Zigbee mode | Zigbee ZCZR (coordinator/router) |
| Partition scheme | Zigbee ZCZR 4MB with spiffs |
| Erase all flash before upload | Enabled, first upload only |

With `arduino-cli`:

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32h2:PartitionScheme=zigbee_zczr,ZigbeeMode=zczr" h2_range_extender
```

```bash
arduino-cli upload --fqbn "esp32:esp32:esp32h2:PartitionScheme=zigbee_zczr,ZigbeeMode=zczr,EraseFlash=all" -p <port> h2_range_extender
```

If the upload does not start, hold **BOOT** while plugging in the cable.

## Zigbee2MQTT setup

1. In the Zigbee2MQTT UI open **Settings → Dev console → External converters**.
2. Create `h2_range_extender.mjs` and paste the contents of [`z2m/h2_range_extender.mjs`](z2m/h2_range_extender.mjs).
3. Save and restart Zigbee2MQTT.
4. Enable **Permit join**, then power the board. The LED blinks blue until it has joined.

### Replacing a board that runs the Arduino example

The example and this firmware have different Zigbee endpoints, so the old pairing cannot be kept:

1. Remove the old device in Zigbee2MQTT.
2. Flash with **Erase all flash before upload** enabled.
3. Pair it again as above.

Devices that were routed through the old extender find a new route on their own. Battery devices can take a few hours.

To pair a board again later, hold **BOOT** for 3 seconds.

## Configuration in the sketch

The values at the top of [`h2_range_extender.ino`](h2_range_extender/h2_range_extender.ino):

| Define | Default | Purpose |
|---|---|---|
| `DEBUG_LOG` | `0` | Set to `1` for USB serial logging. Needs **USB CDC On Boot: Enabled** |
| `TX_POWER_DBM` | `20` | Transmit power, -24 to 20 dBm |
| `MAX_CHILDREN` | `20` | End devices that can attach to this router |
| `LED_BRIGHTNESS` | `24` | 0 to 255 |
| `LED_WHEN_JOINED` | `0` | `1` = dim green while joined and linked |
| `BATTERY_PIN` | `1` | GPIO on the middle of the divider |
| `BATTERY_DIVIDER_RATIO` | `2.0` | (R1 + R2) / R2 |
| `BATTERY_CAL` | `1.0` | Multimeter voltage divided by reported voltage |
| `BATTERY_CUTOFF_MV` | `3300` | Below this the device sleeps |
| `BATTERY_RESUME_MV` | `3600` | Above this a sleeping device starts again |
| `BATTERY_SLEEP_MIN` | `10` | How often a sleeping device looks at the cell |
| `REPORT_INTERVAL_S` | `300` | How often the diagnostics are sent |
| `LINK_WATCHDOG_MIN` | `30` | Restart after this long without a link. `0` switches it off |

## How it works

The Zigbee stack does the routing on its own task. The sketch's `loop()` never waits for the network; it only:

1. Sets the transmit power once the stack runs.
2. Reads the neighbour table every 15 seconds: end devices that have this router as their parent are **children**, routers and the coordinator that sent a link status within the last three periods are **router links**.
3. Restarts the device when there has been no router link for `LINK_WATCHDOG_MIN`. The pairing is kept, so it comes back on its own once the network is there again.
4. Reads the battery every 20 seconds and goes to sleep on an empty cell.
5. Sends the diagnostics.
6. Drives the LED and watches the BOOT button.

### Zigbee endpoints

| Endpoint | Content |
|---|---|
| 1 | Range extender (basic, identify) |
| 2 | Children (analog input) |
| 3 | Router links (analog input) |
| 4 | Uptime in minutes (analog input) |
| 5 | Restart reason (analog input) |
| 6 | Battery voltage in volts (analog input) |
| 7 | Battery level in percent (analog input) |

## Things to know

- **Placement:** a router helps most halfway between the coordinator and the far devices, not next to either.
- **Antenna:** the Super Mini has only its small on-board antenna and no antenna connector. The one this was built on has a 2.4 GHz FPC patch antenna instead, its coax soldered to the board's antenna feed and the patch stuck inside the lid of the case. Either way: keep the antenna away from the cell, from metal and from the USB charger's body where possible.
- **While the coordinator is down** the device restarts every `LINK_WATCHDOG_MIN` if no other router is in range. That is harmless.
- **Calibration:** compare the reported voltage with a multimeter and adjust `BATTERY_CAL`. To keep a board's own value out of the sketch, add `--build-property "compiler.cpp.extra_flags=-DBATTERY_CAL=1.018f"` to the `arduino-cli compile` command.
- **The cutoff only stops the firmware.** It does not disconnect the cell, so use a protected cell.
- **Firmware and converter belong together:** the converter tells the readings apart by endpoint number.
