// ESP32-H2 Super Mini Zigbee range extender (Zigbee router, USB powered, LiPo cell as backup)
//   GPIO1 -> battery voltage through a resistor divider
//
// Based on the Zigbee_Range_Extender example of arduino-esp32. What it does on top of it:
//   - sets the transmit power instead of leaving it at the stack's default
//   - shows on the LED whether it is searching, joined, or joined but alone
//   - restarts when it has had no link to another router or the coordinator for a long time,
//     and when the main loop or the Zigbee stack hangs
//   - reports how many devices use it, how many routers it hears, its uptime and why it
//     last restarted, so Home Assistant shows whether it is doing its job
//   - reports the backup cell's voltage and level, and switches itself off before the cell
//     is drained when the USB power stays away
//
// Arduino IDE / arduino-cli settings (arduino-esp32 >= 3.3):
//   Board:            ESP32H2 Dev Module
//   Zigbee mode:      Zigbee ZCZR (coordinator/router)
//   Partition scheme: Zigbee ZCZR 4MB with spiffs
//   Erase all flash:  Enabled on the first upload only
//
// Zigbee endpoints (matched by z2m/h2_range_extender.mjs):
//   1  range extender (basic, identify)
//   2  analog in: end devices attached to this router (children)
//   3  analog in: routers and coordinator with a live link
//   4  analog in: uptime [min]
//   5  analog in: reason of the last restart (esp_reset_reason_t)
//   6  analog in: battery voltage [V]
//   7  analog in: battery level [%]

#ifndef ZIGBEE_MODE_ZCZR
#error "Zigbee coordinator/router mode is not selected in Tools->Zigbee mode"
#endif

#include "Zigbee.h"
#include "esp_system.h"

/************************ User settings *****************************/
#define DEBUG_LOG 0  // 1 = print to USB serial (needs "USB CDC On Boot: Enabled")

#define TX_POWER_DBM 20  // transmit power, -24 to 20 dBm on the ESP32-H2
#define MAX_CHILDREN 20  // end devices that can attach to this router (the stack's default is 10)

#define LED_BRIGHTNESS  24  // 0 to 255, the RGB LED is very bright
#define LED_WHEN_JOINED 0   // 1 = dim green while everything is fine, 0 = LED off

#define BATTERY_PIN           1     // BAT+ --[R1]--+--[R2]-- GND, GPIO1 on the middle
#define BATTERY_DIVIDER_RATIO 2.0f  // (R1 + R2) / R2, 2.0 for two equal resistors
#define BATTERY_CAL           1.0f  // multimeter voltage / reported voltage
#define BATTERY_CUTOFF_MV     3300  // below this the device sleeps to protect the cell
#define BATTERY_RESUME_MV     3600  // and it starts again once the cell is back above this
#define BATTERY_SLEEP_MIN     10    // how often a sleeping device looks at the cell

#define REPORT_INTERVAL_S 300  // how often the diagnostics are sent

// Restart when no router or coordinator has been heard for this long. A restart keeps the
// pairing. 0 switches the watchdog off.
#define LINK_WATCHDOG_MIN 30

#define FACTORY_RESET_HOLD_MS 3000  // hold BOOT this long to leave the network and pair again

/************************ Fixed values *****************************/
#define EP_EXTENDER 1
#define EP_CHILDREN 2
#define EP_ROUTERS  3
#define EP_UPTIME   4
#define EP_RESTART  5
#define EP_VOLTAGE  6
#define EP_BATTERY  7

#define LINK_CHECK_INTERVAL_S 15  // one link status period of the Zigbee network layer
#define ROUTER_AGE_LIMIT      3   // link status periods without a word before a router counts as gone
#define FIRST_REPORT_S        30  // first diagnostics after a start

#define BATTERY_CHECK_INTERVAL_S 20
#define BATTERY_PRESENT_MV       2500  // less than this on the divider: no cell and no divider fitted
#define BATTERY_LOW_READINGS     3     // readings in a row below the cutoff before the device sleeps

// Logging compiles to nothing unless DEBUG_LOG is set
#if DEBUG_LOG
#define LOG(...) Serial.printf(__VA_ARGS__)
#else
#define LOG(...)
#endif

ZigbeeRangeExtender zbExtender = ZigbeeRangeExtender(EP_EXTENDER);
ZigbeeAnalog zbChildren = ZigbeeAnalog(EP_CHILDREN);
ZigbeeAnalog zbRouters = ZigbeeAnalog(EP_ROUTERS);
ZigbeeAnalog zbUptime = ZigbeeAnalog(EP_UPTIME);
ZigbeeAnalog zbRestart = ZigbeeAnalog(EP_RESTART);
ZigbeeAnalog zbVoltage = ZigbeeAnalog(EP_VOLTAGE);
ZigbeeAnalog zbBattery = ZigbeeAnalog(EP_BATTERY);

uint8_t children = 0;           // end devices attached to this router
uint8_t routers = 0;            // routers and coordinator with a live link
uint32_t lastLinkMs = 0;        // last time a router or the coordinator was heard
uint32_t identifyUntilMs = 0;   // the LED blinks white until then
bool txPowerSet = false;

uint16_t batteryMv = 0;
uint8_t batteryPct = 0;
bool batteryValid = false;      // false when nothing is on the divider

/************************ LED *****************************/
// The Super Mini has a WS2812 RGB LED on GPIO8
void led(uint8_t red, uint8_t green, uint8_t blue) {
  static uint32_t shown = 0xFFFFFFFF;
  uint32_t colour = ((uint32_t)red << 16) | ((uint32_t)green << 8) | blue;
  if (colour == shown) {
    return;
  }
  shown = colour;
  rgbLedWrite(RGB_BUILTIN, red, green, blue);
}

// white blink  = identify (from Zigbee2MQTT)
// blue blink   = not on a network, searching (enable Permit join)
// red blink    = on the network, but no router or coordinator in range
// off / green  = joined and linked
void updateLed() {
  uint32_t now = millis();
  bool blink = (now / 500) % 2;
  uint8_t on = LED_BRIGHTNESS;
  if (now < identifyUntilMs) {
    led(blink ? on : 0, blink ? on : 0, blink ? on : 0);
  } else if (!Zigbee.connected()) {
    led(0, 0, blink ? on : 0);
  } else if (routers == 0) {
    led(blink ? on : 0, 0, 0);
  } else {
    led(0, LED_WHEN_JOINED ? on / 4 : 0, 0);
  }
}

// Called once a second while the device is asked to identify itself, with the seconds left
void onIdentify(uint16_t secondsLeft) {
  identifyUntilMs = secondsLeft ? millis() + 1500 : 0;
}

/************************ Battery *****************************/
// Average of 8 ADC samples, scaled back up by the divider ratio to the real cell voltage
uint16_t readBatteryMillivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogReadMilliVolts(BATTERY_PIN);
  }
  return (uint16_t)((sum / 8.0f) * BATTERY_DIVIDER_RATIO * BATTERY_CAL);
}

// Voltage to charge level for a single Li-ion / LiPo cell at rest: {millivolts, percent}
// points, linearly interpolated in between. On USB power the cell is being charged and
// reads higher than its real level.
uint8_t batteryPercent(uint16_t mv) {
  static const uint16_t curve[][2] = {{4150, 100}, {4050, 90}, {3970, 80}, {3900, 70}, {3840, 60}, {3790, 50},
                                      {3750, 40},  {3710, 30}, {3670, 20}, {3600, 10}, {3450, 5},  {3300, 0}};
  const int n = sizeof(curve) / sizeof(curve[0]);
  if (mv >= curve[0][0]) {
    return 100;
  }
  for (int i = 1; i < n; i++) {
    if (mv >= curve[i][0]) {
      return curve[i][1] + (uint32_t)(mv - curve[i][0]) * (curve[i - 1][1] - curve[i][1]) / (curve[i - 1][0] - curve[i][0]);
    }
  }
  return 0;
}

void readBattery() {
  batteryMv = readBatteryMillivolts();
  batteryValid = batteryMv >= BATTERY_PRESENT_MV;
  batteryPct = batteryValid ? batteryPercent(batteryMv) : 0;
}

// Radio and LED off, and look at the cell again later. Every wake-up is a fresh boot.
void sleepOnEmptyBattery() {
  LOG("Battery %u mV, sleeping\n", batteryMv);
  led(0, 0, 0);
  esp_sleep_enable_timer_wakeup(BATTERY_SLEEP_MIN * 60ULL * 1000000ULL);
  esp_deep_sleep_start();
}

// A router cannot save power: its radio has to listen all the time. So on a cell that is
// nearly empty the only thing left is to stop, until the USB power is back and has charged
// the cell a little.
void checkBattery() {
  static uint8_t lowReadings = 0;
  readBattery();
  if (!batteryValid || batteryMv >= BATTERY_CUTOFF_MV) {
    lowReadings = 0;
  } else if (++lowReadings >= BATTERY_LOW_READINGS) {
    sleepOnEmptyBattery();
  }
}

/************************ Links *****************************/
// Count the neighbours: the end devices that have this router as their parent, and the
// routers (and coordinator) that sent a link status lately. Without one of those nothing
// can be forwarded.
void readNeighbours() {
  uint8_t childCount = 0, routerCount = 0;
  esp_zb_nwk_info_iterator_t iterator = ESP_ZB_NWK_INFO_ITERATOR_INIT;
  esp_zb_nwk_neighbor_info_t neighbour;

  esp_zb_lock_acquire(portMAX_DELAY);
  while (esp_zb_nwk_get_next_neighbor(&iterator, &neighbour) == ESP_OK) {
    if (neighbour.device_type == ESP_ZB_DEVICE_TYPE_ED) {
      if (neighbour.relationship == ESP_ZB_NWK_RELATIONSHIP_CHILD) {
        childCount++;
      }
    } else if (neighbour.age <= ROUTER_AGE_LIMIT) {
      routerCount++;
    }
  }
  esp_zb_lock_release();

  if (childCount != children || routerCount != routers) {
    LOG("Children: %u, routers: %u\n", childCount, routerCount);
  }
  children = childCount;
  routers = routerCount;
}

// The stack does not notice when the network has gone for good (coordinator replaced, device
// removed while it was off, radio stuck): it keeps believing it is joined. So when nothing
// has been heard for a long time, start over. The pairing survives a restart.
void checkLinks() {
  readNeighbours();
  if (routers > 0 || !Zigbee.connected()) {
    lastLinkMs = millis();
    return;
  }
#if LINK_WATCHDOG_MIN > 0
  if (millis() - lastLinkMs > LINK_WATCHDOG_MIN * 60000UL) {
    LOG("No link for %u min, restarting\n", LINK_WATCHDOG_MIN);
    delay(100);
    ESP.restart();
  }
#endif
}

/************************ Radio *****************************/
// The transmit power can only be set once the stack runs
void setTxPower() {
  esp_zb_lock_acquire(portMAX_DELAY);
  esp_zb_set_tx_power(TX_POWER_DBM);
  int8_t power = 0;
  esp_zb_get_tx_power(&power);
  esp_zb_lock_release();
  LOG("Transmit power: %d dBm\n", power);
  txPowerSet = true;
}

/************************ Reports *****************************/
void reportDiagnostics() {
  zbChildren.setAnalogInput(children);
  zbRouters.setAnalogInput(routers);
  zbUptime.setAnalogInput(millis() / 60000);
  zbRestart.setAnalogInput((float)esp_reset_reason());
  zbVoltage.setAnalogInput(batteryMv / 1000.0f);
  zbBattery.setAnalogInput(batteryPct);
  zbChildren.reportAnalogInput();
  zbRouters.reportAnalogInput();
  zbUptime.reportAnalogInput();
  zbRestart.reportAnalogInput();
  zbVoltage.reportAnalogInput();
  zbBattery.reportAnalogInput();
}

void addDiagnostic(ZigbeeAnalog &endpoint, const char *description, float resolution = 1) {
  endpoint.addAnalogInput();
  endpoint.setAnalogInputDescription(description);
  endpoint.setAnalogInputResolution(resolution);
  Zigbee.addEndpoint(&endpoint);
}

/************************ Button *****************************/
// Holding BOOT leaves the network and restarts, ready to be paired again
void checkButton() {
  if (digitalRead(BOOT_PIN) == HIGH) {
    return;
  }
  uint32_t pressed = millis();
  while (digitalRead(BOOT_PIN) == LOW) {
    led((millis() / 100) % 2 ? LED_BRIGHTNESS : 0, 0, 0);
    if (millis() - pressed > FACTORY_RESET_HOLD_MS) {
      LOG("Leaving the network\n");
      led(LED_BRIGHTNESS, 0, 0);
      delay(500);
      Zigbee.factoryReset(true);
    }
    delay(20);
  }
}

/************************ Arduino *****************************/
void setup() {
#if DEBUG_LOG
  Serial.begin(115200);
  delay(1500);
  LOG("Range extender starting, restart reason %d\n", (int)esp_reset_reason());
#endif

  pinMode(BOOT_PIN, INPUT_PULLUP);
  led(0, 0, 0);

  // Before the radio starts: stay asleep on an empty cell. After a sleep the cell has to be
  // well above the cutoff, or the device would switch on and off as the voltage recovers.
  readBattery();
  bool wokeFromSleep = esp_reset_reason() == ESP_RST_DEEPSLEEP;
  if (batteryValid && batteryMv < (wokeFromSleep ? BATTERY_RESUME_MV : BATTERY_CUTOFF_MV)) {
    sleepOnEmptyBattery();
  }

  zbExtender.setManufacturerAndModel("CustomDIY", "H2_Range_Extender");
  zbExtender.setPowerSource(ZB_POWER_SOURCE_MAINS);
  zbExtender.onIdentify(onIdentify);
  Zigbee.addEndpoint(&zbExtender);

  addDiagnostic(zbChildren, "Children");
  addDiagnostic(zbRouters, "Router links");
  addDiagnostic(zbUptime, "Uptime (min)");
  addDiagnostic(zbRestart, "Restart reason");
  addDiagnostic(zbVoltage, "Battery voltage (V)", 0.001);
  addDiagnostic(zbBattery, "Battery (%)");

  esp_zb_cfg_t zigbeeConfig = ZIGBEE_DEFAULT_ROUTER_CONFIG();
  zigbeeConfig.nwk_cfg.zczr_cfg.max_children = MAX_CHILDREN;

  if (!Zigbee.begin(&zigbeeConfig)) {
    LOG("Zigbee failed to start, restarting\n");
    delay(1000);
    ESP.restart();
  }

  // From here on a hanging loop() restarts the device. loop() also takes the Zigbee lock,
  // so a hanging stack does the same.
  enableLoopWDT();
  lastLinkMs = millis();
}

// The stack routes on its own task. loop() only looks after the LED, the button, the
// battery, the link watchdog and the diagnostics, and never waits for the network.
void loop() {
  static uint32_t lastCheckMs = 0;
  static uint32_t lastReportMs = 0;
  static uint32_t lastBatteryMs = 0;
  static bool reported = false;
  uint32_t now = millis();

  checkButton();

  if (!txPowerSet && Zigbee.started()) {
    setTxPower();
  }

  if (now - lastBatteryMs >= BATTERY_CHECK_INTERVAL_S * 1000UL) {
    lastBatteryMs = now;
    checkBattery();
  }

  if (now - lastCheckMs >= LINK_CHECK_INTERVAL_S * 1000UL) {
    lastCheckMs = now;
    checkLinks();
  }

  if (Zigbee.connected()) {
    uint32_t interval = (reported ? REPORT_INTERVAL_S : FIRST_REPORT_S) * 1000UL;
    if (now - lastReportMs >= interval) {
      lastReportMs = now;
      reported = true;
      readNeighbours();
      reportDiagnostics();
    }
  } else {
    lastReportMs = now;  // the first report goes out a little after joining
    reported = false;
  }

  updateLed();
  delay(20);
}
