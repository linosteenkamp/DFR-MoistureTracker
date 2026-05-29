// zigbee2mqtt external converter for the DFR-MoistureTracker DIY soil sensor
// (ESP32-C6, esp-zigbee 1.6.x). Exposes:
//   - battery          (Power Configuration cluster 0x0001)
//   - soil_moisture %  (carried on the Relative Humidity cluster 0x0405, since the
//                       SDK's custom Soil Moisture 0x0408 cluster asserts; humidity
//                       and soil moisture share the same 0.01% uint16 wire format)
//
// Install: copy this file into the zigbee2mqtt config dir, reference it under
//   external_converters:
//     - dfr_soil_moisture.js
// (older z2m) or drop it in the `external_converters` folder (newer z2m), then
// restart zigbee2mqtt. The device's modelID "DFR-SoilSensor" (read during the
// interview) matches `zigbeeModel`, so no re-pair is needed — a restart re-applies
// the definition.

const {battery, numeric} = require('zigbee-herdsman-converters/lib/modernExtend');

module.exports = [
    {
        zigbeeModel: ['DFR-SoilSensor'],
        model: 'DFR-SoilSensor',
        vendor: 'DFRobot-DIY',
        description: 'ESP32-C6 soil moisture + battery sensor (DIY)',
        extend: [
            battery({
                percentage: true,
                voltage: true,
                // Battery attrs are on Power Config (0x0001): 0x0021 (percentage,
                // 0.5% units) and 0x0020 (voltage, 100 mV units) — both standard,
                // handled by the built-in battery() extend.
            }),
            numeric({
                name: 'soil_moisture',
                cluster: 'msRelativeHumidity',          // 0x0405
                attribute: 'measuredValue',
                // measuredValue is in 0.01% units (0..10000); divide by 100 -> %.
                scale: 100,
                unit: '%',
                precision: 1,
                description: 'Soil moisture',
                access: 'STATE',
                reporting: {min: '10_SECONDS', max: '1_HOUR', change: 50},
            }),
        ],
    },
];
