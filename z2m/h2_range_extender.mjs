// Zigbee2MQTT external converter for the ESP32-H2 range extender
// (firmware: ../h2_range_extender/h2_range_extender.ino)
//
// What it does:
//   - gives the router a name instead of "unsupported device"
//   - turns its reports into children, router links, uptime and restart reason
//
// Install: Zigbee2MQTT UI -> Settings -> Dev console -> External converters -> create
// "h2_range_extender.mjs", paste this file, save, restart Zigbee2MQTT.

import * as exposes from 'zigbee-herdsman-converters/lib/exposes';

const e = exposes.presets;
const ea = exposes.access;

// esp_reset_reason_t of ESP-IDF, by its number
const RESTART_REASONS = [
    'unknown',
    'power_on',
    'external_reset',
    'software',
    'crash',
    'interrupt_watchdog',
    'task_watchdog',
    'other_watchdog',
    'deep_sleep',
    'brownout',
    'sdio',
    'usb',
    'jtag',
    'efuse',
    'power_glitch',
    'cpu_lockup',
];

// Device -> Zigbee2MQTT
const fzLocal = {
    // Float readings, told apart by the endpoint they arrive on
    analog_input: {
        cluster: 'genAnalogInput',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg) => {
            if (msg.data.presentValue === undefined) return;
            const value = Math.round(msg.data.presentValue);
            switch (msg.endpoint.ID) {
                case 2:
                    return {children: value};
                case 3:
                    return {router_links: value};
                case 4:
                    return {uptime: value};
                case 5:
                    return {restart_reason: RESTART_REASONS[value] ?? 'unknown'};
            }
        },
    },
};

export default {
    zigbeeModel: ['H2_Range_Extender'],
    model: 'H2_Range_Extender',
    vendor: 'CustomDIY',
    description: 'ESP32-H2 Super Mini Zigbee range extender (router)',
    fromZigbee: [fzLocal.analog_input],
    toZigbee: [],
    exposes: [
        e.numeric('children', ea.STATE).withDescription('End devices that are attached to this router').withCategory('diagnostic'),
        e
            .numeric('router_links', ea.STATE)
            .withDescription('Routers (and coordinator) this router has a live link to. 0 means it cannot forward anything')
            .withCategory('diagnostic'),
        e.numeric('uptime', ea.STATE).withUnit('min').withDescription('Time since the last restart').withCategory('diagnostic'),
        e.enum('restart_reason', ea.STATE, RESTART_REASONS).withDescription('Why the device last restarted').withCategory('diagnostic'),
    ],
    // Runs once after pairing: bind the diagnostics to the coordinator. The firmware sends
    // its reports to bound targets only, so nothing arrives without these binds.
    configure: async (device, coordinatorEndpoint) => {
        for (const id of [2, 3, 4, 5]) {
            await device.getEndpoint(id).bind('genAnalogInput', coordinatorEndpoint);
        }
    },
};
