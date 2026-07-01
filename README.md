# BMS

ESP32 firmware for reading data from a battery management system over UART, displaying telemetry in a web interface, and optionally sending CAN data to a Deye inverter.

## Features

- Reads BMS data over Modbus/UART
- Shows a dashboard and diagnostics in the browser
- Exposes JSON for Home Assistant and other integrations
- Supports saving settings from the web interface
- Includes a firmware upload page for OTA updates

## Startup

1. Connect the ESP32 to the BMS and, if needed, to the inverter CAN port.
2. Flash the firmware using PlatformIO.
3. After boot, the device will try to connect to the saved Wi-Fi network.
4. If Wi-Fi is not configured or the connection fails, the device starts an access point:
   - SSID: `BMS`
   - IP: `192.168.0.1`

## Wiring

### UART to BMS

Default pins:

- `BMS DE` - GPIO `4`
- `BMS RX` - GPIO `16`
- `BMS TX` - GPIO `17`

UART runs at `9600 8N1`.

### CAN to inverter

If CAN output is enabled, the default pins are:

- `CAN RX` - GPIO `18`
- `CAN TX` - GPIO `19`

CAN runs at `500 kbps`.

## Web Interface

- Open the device address in a browser
- Go to `Settings`
- Configure Wi-Fi, pins, BMS ID, and other options
- Upload a new firmware image from the `Firmware update` section if needed

## Build

To build locally:

```bash
pio run -e esp32doit-devkit-v1
```

The resulting firmware file will be here:

```text
.pio/build/esp32doit-devkit-v1/firmware.bin
```

## Home Assistant

An example configuration is included:

- [`home_assistant_bms_example.yaml`](home_assistant_bms_example.yaml)

It uses the `/json` endpoint.

## Notes

The available pins and runtime behavior can be changed from `Settings`, so after the first boot it is a good idea to check the web interface and save your own configuration.
