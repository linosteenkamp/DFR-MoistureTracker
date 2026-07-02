// zigbee2mqtt external converter for the DFR-MoistureTracker DIY soil sensor
// (ESP32-C6, esp-zigbee 1.6.x). Exposes:
//   - battery          (Power Configuration cluster 0x0001)
//   - soil_moisture %  (carried on the Relative Humidity cluster 0x0405, since the
//                       SDK's custom Soil Moisture 0x0408 cluster asserts; humidity
//                       and soil moisture share the same 0.01% uint16 wire format)
//   - label            (device-set sensor name, from Basic cluster
//                       LocationDescription 0x0010; published in every payload so
//                       Node-RED can identify the physical sensor)
//
// Install: copy this file into the zigbee2mqtt config dir, reference it under
//   external_converters:
//     - dfr_soil_moisture.js
// (older z2m) or drop it in the `external_converters` folder (newer z2m), then
// restart zigbee2mqtt. The device's modelID "DFR-SoilSensor" matches zigbeeModel,
// so no re-pair is needed — a restart re-applies the definition. After updating,
// trigger a re-interview / "reconfigure" in z2m so the configure() below reads
// locationDesc.

// OTA: in zigbee2mqtt configuration.yaml set:
//   ota:
//     zigbee_ota_override_index_location: https://raw.githubusercontent.com/linosteenkamp/DFR-MoistureTracker/master/ota/index.json
//     disable_automatic_update_check: true   # staged/manual rollout
// Trigger updates per device in the z2m UI (device -> OTA -> Update).

const {battery, numeric} = require('zigbee-herdsman-converters/lib/modernExtend');
const exposes = require('zigbee-herdsman-converters/lib/exposes');
const e = exposes.presets;
const ea = exposes.access;

const fzLabel = {
    cluster: 'genBasic',
    type: ['attributeReport', 'readResponse'],
    convert: (model, msg, publish, options, meta) => {
        if (msg.data.locationDesc !== undefined) {
            return {label: msg.data.locationDesc};
        }
    },
};

module.exports = [
    {
        zigbeeModel: ['DFR-SoilSensor'],
        model: 'DFR-SoilSensor',
        vendor: 'DFRobot-DIY',
        description: 'ESP32-C6 soil moisture + battery sensor (DIY)',
        // Bespoke line icon (droplet + soil, teal #0e9aa7) embedded as a data-URI so
        // it ships with the definition. Source SVG: z2m/dfr_soil_moisture.svg.
        icon: 'data:image/svg+xml;base64,PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHZpZXdCb3g9IjAgMCAyNCAyNCIgZmlsbD0ibm9uZSIgc3Ryb2tlPSIjMGU5YWE3IiBzdHJva2Utd2lkdGg9IjEuNyIgc3Ryb2tlLWxpbmVjYXA9InJvdW5kIiBzdHJva2UtbGluZWpvaW49InJvdW5kIj4KICA8cGF0aCBkPSJNMTIgMi41QzEyIDIuNSA3IDggNyAxMS41YTUgNSAwIDAgMCAxMCAwQzE3IDggMTIgMi41IDEyIDIuNVoiLz4KICA8cGF0aCBkPSJNMy41IDE5YzItMS4xIDQtMS4xIDYgMHM0IDEuMSA2IDAiLz4KICA8cGF0aCBkPSJNMy41IDIxLjVjMi0xLjEgNC0xLjEgNiAwczQgMS4xIDYgMCIvPgo8L3N2Zz4K',
        extend: [
            battery({
                percentage: true,
                voltage: true,
            }),
            numeric({
                name: 'soil_moisture',
                cluster: 'msRelativeHumidity',          // 0x0405
                attribute: 'measuredValue',
                scale: 100,                              // 0.01% units -> %
                unit: '%',
                precision: 1,
                description: 'Soil moisture',
                access: 'STATE',
                reporting: {min: '10_SECONDS', max: '1_HOUR', change: 50},
            }),
        ],
        fromZigbee: [fzLabel],
        exposes: [
            e.text('label', ea.STATE).withDescription('Device-set sensor name (Node-RED identifier)'),
        ],
        configure: async (device, coordinatorEndpoint, logger) => {
            const ep = device.getEndpoint(1);
            await ep.read('genBasic', ['locationDesc']);
        },
        // OTA (z2m 2.x form): `ota: true` opts the device into z2m's OTA subsystem,
        // which matches an image from the override index by manufacturerCode 0xFEFE
        // + imageType 0x0001 (see ota/index.json + zigbee_ota_override_index_location).
        // NOTE: z2m 1.x used `ota: ota.zigbeeOTA` with a lib/ota require instead.
        ota: true,
    },
];
