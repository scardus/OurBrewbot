# OurBrewbot Firmware

Replacement firmware for the MyBrewbot ESP8266 fermentation controller.

---

## What this does

The original firmware depended on `mybrewbot.co.uk` for cloud
storage, the Blynk App, and OTA updates. All of those are now dead.

This firmware keeps your hardware working:
- All temperature probe control preserved (DS18B20 probes, same addressing)
- Smart plug RF transmit preserved (same codes, same protocol)
- Fermentation profile runner preserved
- Brewfather / Brewer's Friend integrations with per-fermenter toggles
- All JSON config files 100% compatible with original (same field names)
- iSpindel HTTP receiver
- OTA firmware update via web browser
- Tilt hydrometer support via HM-10 BLE module (standard and Tilt Pro)

New/Updated Features:
- Tilt Pro support — auto-detected by broadcast value magnitude; gravity and temperature decoded with correct Pro precision (÷10000 / ÷10), labelled "Pro" in the admin UI
- Brewfather integration uses the Custom Stream API
- MQTT support for publishing fermenter data to any MQTT broker
- Home Assistant MQTT Discovery — auto-creates HA entities with no YAML needed (optional, per-broker toggle)
- Home Assistant MQTT control - see section below
- Full local REST API
- Web-based admin page for configuring probes, fermenters, Tilts, iSpindels, plugs, and services
- mDNS — device registers as `ourbrewbot-CHIPID.local` on the local network
- LittleFS file browser in admin page for inspecting config files
- BLE AT command console for debugging HM-10 Bluetooth module
- Rebuilt Fermentation Profiles tab — 4 editable profiles with up to 15 steps each, per-fermenter assignment, start/stop/pause, manual step navigation

Not yet implemented / tested:
- Pressure sensor - __Untested - No hardware__
- Alarms - No mobile app or cloud service, so nothing to send alarms to. __Currently using MQTT__

Removed:
- mybrewbot.co.uk cloud backend (server gone)
- Blynk dashboard (replaced with REST API + admin page)

---

## How this was done

The hardware connections were traced with a simple multimeter.  Analysis of the previous firmware image was performed by Claude Code.  Reverse engineering of the machine code was unfeasable, so instead we pulled generic information from the JSON configuration files and any function name strings/symbols it could find.  From this, it was possible to build a basic framework to iterate upon and re-build features.

---

## Hardware

Pin assignments are in `Pins.h` using raw GPIO numbers (compatible with both
NodeMCU and generic ESP8266 board targets). 

| Function       | GPIO  | NodeMCU Pin | Label          |
|----------------|-------|-------------|----------------|
| Probe Bus 1    | GPIO0 | D3          | Green Jack     |
| Probe Bus 2    | GPIO2 | D4          | Black Jack     |
| RF Transmitter | GPIO4 | D2          | FS1000A TX     |
| RF Receiver    | GPIO14 | D5          | MX-RM-5V RX    |
| BLE (ESP TX)   | GPIO12| D6          | HM-10 BT 4.0 RX ← ESP TX |
| BLE (ESP RX)   | GPIO13| D7          | HM-10 BT 4.0 TX → ESP RX |
| LED            | GPIO16| D0          | NodeMCU LED_BUILTIN |

---

## Building

### 1. Install VS Code

Download and install from https://code.visualstudio.com/

### 2. Install the PlatformIO IDE extension

In VS Code: open the Extensions panel (`Ctrl+Shift+X`), search for **PlatformIO IDE**, and install it. Restart VS Code when prompted.

### 3. Open the project

Open the repository root folder in VS Code (`File → Open Folder`). PlatformIO will detect `platformio.ini` automatically and install all required libraries and the ESP8266 toolchain on first open.

### 4. Configure the upload port

In `platformio.ini`, set `upload_port` and `monitor_port` to match your device's COM port (currently `COM7`). Adjust if your port differs.

### 5. Build and upload

- **Build only:** click the checkmark (✓) in the PlatformIO toolbar, or run `PlatformIO: Build` from the command palette (`Ctrl+Alt+B`).
- **Build and upload:** click the right-arrow (→) in the toolbar, or run `PlatformIO: Upload` (`Ctrl+Alt+U`).
- **Serial monitor:** click the plug icon in the toolbar, or run `PlatformIO: Monitor`.

---

## Backing up your original firmware & settings

Before flashing, back up the full 4 MB flash from your existing MyBrewbot device so you can restore it if needed.

Your config files (probes, fermenters, etc.) live in the LittleFS partition and survive a firmware-only flash — but a full backup protects everything.

### Put the device in bootloader mode

Hold the **FLASH** button, press and release **RST**, then release **FLASH**. On NodeMCU boards the USB adapter usually handles this automatically via DTR/RTS when you connect it.

### Option A — esptool.py (cross-platform)

```bash
pip install esptool
esptool.py --port COM7 --baud 115200 read_flash 0x0 0x400000 mybrewbot_backup.bin
```

> `COM7` is an example — check Device Manager (Windows) or `ls /dev/ttyUSB*` (Linux/Mac) for your actual port.

The result is a 4 MB `.bin` file — store it somewhere safe.

### Option B — Espressif Flash Download Tool (Windows GUI)

Download and install the tool from the [Espressif Flash Download Tool documentation](https://docs.espressif.com/projects/esp-test-tools/en/latest/esp8266/production_stage/tools/flash_download_tool.html).

1. Run `flash_download_tool_x.x.x.exe`
2. Select **ESP8266** / **Develop** / **UART**
3. Open the **chipInfoDump** tab
4. Set start address `0x0` and length `0x400000`
5. Select your COM port and click **READ** — the tool saves the backup as a `.bin` automatically

---

## Flashing the new firmware

Pre-built binaries are in the `bin/` folder of this repository. You do **not** need VS Code or PlatformIO installed — just the binary and one of the tools below.

Put the device in bootloader mode as described above before flashing.

### Option A — esptool.py (cross-platform)

```bash
esptool.py --port COM7 --baud 115200 write_flash -fm dout 0x0 OurBrewbot_x.x.x.bin
```

> Replace `COM7` with your actual port and `OurBrewbot_x.x.x.bin` with the filename from the `bin/` folder.

### Option B — Espressif Flash Download Tool (Windows GUI)

1. Run the tool, select **ESP8266** / **Develop** / **UART**
2. Click the **SPIDownload** tab
3. Tick the checkbox on the first row, click `...` to browse to `OurBrewbot_x.x.x.bin`, and set the address to `0x0`
4. Set **SPI Speed** to `40MHz` and **SPI Mode** to `DOUT`
5. Select your COM port and baud rate to 115200
6. Click **START**

### Option C — OTA (no cable, after initial flash)

Once the firmware is running, navigate to `http://OurBrewbot-XXXXXX/update` in your browser and upload the `.bin` file directly.

---

## First Run

1. On first boot the device creates a WiFi access point named `OurBrewbot-XXXXXX` - __Make a note of this!__
2. Connect to it from your phone or laptop
3. A configuration portal opens — enter your WiFi SSID and password
4. The device reboots, connects to your network and registers it's name in mDNS
5. Open http://OurBrewbot-XXXXXX (noted in step 1) in your browser.  If this does not work, find your device's IP from your router admin page, then open `http://DEVICEIP/` in a browser.

---

## Restoring Your Original Config

If you have the original device, its config files are stored in the
LittleFS partition. 

These should be auto-detected and used if you flash this firmware to the same device
(the LittleFS partition is separate and survives firmware updates).

---

## REST API

| Method | Endpoint           | Description                    |
|--------|--------------------|-------------------------------|
| GET    | /                  | Welcome / API index page      |
| GET    | /admin             | Admin configuration page      |
| GET    | /ble/sniff         | BLE AT command console page   |
| GET    | /ble/sniff/poll    | Poll BLE serial data          |
| POST   | /ble/sniff/send    | Send AT command to HM-10      |
| GET    | /board_info.json   | Board info                    |
| GET    | /brewservices      | Brew service config           |
| POST   | /brewservices      | Update brew service config    |
| POST   | /brewservices/test | Test brew service connection  |
| GET    | /config            | WiFi config page              |
| GET    | /configMe          | Save WiFi config (form GET)   |
| GET    | /controller        | Controller config + plugs     |
| POST   | /controller        | Update global config          |
| GET    | /fermenter?id=0    | Single fermenter              |
| POST   | /fermenter         | Update fermenter config       |
| POST   | /fermenter/profile | Profile control (start/stop/pause/next/prev) |
| GET    | /fermenters        | All fermenter data (JSON)     |
| GET    | /fs/file           | Read LittleFS file content    |
| GET    | /fs/files          | List LittleFS files           |
| GET    | /health            | System health                 |
| POST   | /iSpindel          | iSpindel gravity data         |
| GET    | /mqtt              | MQTT config (includes haDiscovery flag) |
| POST   | /mqtt              | Update MQTT config (haDiscovery, LWT, discovery cleanup on disable) |
| POST   | /mqtt/discover     | Trigger HA MQTT discovery     |
| POST   | /mqtt/test         | Test MQTT connection          |
| GET    | /probes            | All temperature probes        |
| POST   | /probes            | Update probe config           |
| POST   | /profile           | Update fermentation profile   |
| GET    | /profiles          | Fermentation profile config   |
| GET    | /reboot            | Reboot device                 |
| GET    | /reset             | Reset all config to defaults  |
| GET    | /rf/sniff          | RF sniff page                 |
| GET    | /rf/sniff/poll     | Poll RF sniff results         |
| POST   | /smartplug         | Update smart plug config      |
| POST   | /smartplug/test    | Test smart plug RF on/off     |
| GET    | /smartplugs        | Smart plug config (JSON)      |
| GET    | /status            | Quick status all fermenters   |
| GET    | /syslog            | Syslog config                 |
| POST   | /syslog            | Update syslog config (host, port, facility, minLevel) |
| POST   | /tilt              | Update Tilt config (fermenter, function, SG/temp adjust) |
| GET    | /tilts             | Tilt hydrometer config + live data |
| GET    | /update            | OTA firmware update page      |
| POST   | /update            | Upload new firmware binary    |
| GET    | /WiFi              | WiFi config page (alias)      |

---

## Home Assistant MQTT Integration

When MQTT is enabled and **HA Discovery** is turned on, the device publishes Home Assistant MQTT discovery payloads on connect and whenever HA restarts. No manual YAML configuration is needed — entities appear automatically in HA.

### Entity types (v0.1.74+)

| Entity type | Fields |
|-------------|--------|
| `sensor`    | beer_temperature, ambient_temperature, gravity, gravity_source, attenuation, status, beer_temperature_source, temperature_unit, profile_step, profile_steps |
| `switch`    | power, temp_control, profile_running |
| `number`    | ceiling_temperature, floor_temperature, hysteresis, compressor_delay, og, tg |
| `text`      | name, beer_name, yeast |
| `select`    | profile_no (options: 0–4) |
| `button`    | Device: reboot, all_off |

All temperatures are published in **°C** regardless of display unit setting. Conversion for display is handled by the receiving side (HA, dashboard, etc.).

### Allow HA Control

By default the device is read-only from HA's perspective. Enable **Allow HA Control** in the MQTT settings page to allow Home Assistant to send commands back to the device.

When enabled, the device subscribes to `<baseTopic>/+/+/set` and accepts commands for all writable entity types (switch, number, text, select, button). Commands are validated — out-of-range values are silently rejected and the HA entity reverts to the actual device state within ~60 seconds via the regular state publish.

When disabled, all discovery entities are still advertised (so the dashboard YAML remains stable), but the device ignores any incoming commands.

### Dashboard

A ready-made Lovelace dashboard is provided in `HomeAssistant/dashboard.yaml`. It requires these HACS frontend cards:
- [lovelace-plotly-graph-card](https://github.com/dbuezas/lovelace-plotly-graph-card)
- [lovelace-mushroom](https://github.com/piitaya/lovelace-mushroom)
- [card-mod](https://github.com/thomasloven/lovelace-card-mod)

---

## Upgrading from earlier firmware

### From v0.1.73 or earlier (pre-MQTT control)

Versions before v0.1.74 published all per-fermenter fields as `sensor` entities. From v0.1.74 onwards, several fields changed to more appropriate HA entity types (switch, number, text, select). The firmware sends empty retained payloads to the old discovery topics on connect to clean up stale HA entities automatically.

**Steps after flashing:**

1. Flash the new binary via OTA (`http://ourbrewbot-XXXXXX.local/update`) or esptool.
2. Once connected, the device retries HA discovery automatically. If entities don't update immediately, click **HA Discover** in the MQTT settings page of the admin UI, or reboot the device.
3. HA will remove the old `sensor.*` entities and create the new `switch.*`, `number.*`, `text.*`, and `select.*` entities.
4. Replace your Lovelace dashboard with the updated `HomeAssistant/dashboard.yaml` from this repository. The new YAML references the correct entity types.

**Entity ID changes (old → new):**

| Old entity ID | New entity ID |
|---------------|---------------|
| `sensor.ourbrewbot_fN_power` | `switch.ourbrewbot_fN_power` |
| `sensor.ourbrewbot_fN_temp_control` | `switch.ourbrewbot_fN_temp_control` |
| `sensor.ourbrewbot_fN_profile_running` | `switch.ourbrewbot_fN_profile_running` |
| `sensor.ourbrewbot_fN_ceiling_temperature` | `number.ourbrewbot_fN_ceiling_temperature` |
| `sensor.ourbrewbot_fN_floor_temperature` | `number.ourbrewbot_fN_floor_temperature` |
| `sensor.ourbrewbot_fN_hysteresis` | `number.ourbrewbot_fN_hysteresis` |
| `sensor.ourbrewbot_fN_compressor_delay` | `number.ourbrewbot_fN_compressor_delay` |
| `sensor.ourbrewbot_fN_og` | `number.ourbrewbot_fN_og` |
| `sensor.ourbrewbot_fN_tg` | `number.ourbrewbot_fN_tg` |
| `sensor.ourbrewbot_fN_name` | `text.ourbrewbot_fN_name` |
| `sensor.ourbrewbot_fN_beer_name` | `text.ourbrewbot_fN_beer_name` |
| `sensor.ourbrewbot_fN_yeast` | `text.ourbrewbot_fN_yeast` |
| _(new)_ | `select.ourbrewbot_fN_profile_no` |

> Replace `N` with the fermenter index (0–3).

Any HA automations, template sensors (`binary_sensor.ourbrewbot_fN_online`, `sensor.ourbrewbot_fN_fermentation_progress`, etc.) or Plotly graph history that reference the old `sensor.*` entity IDs will need to be updated to the new IDs.

---

## File Structure

```
OurBrewbot/
  OurBrewbot.cpp       Main sketch — setup, loop, state machine
  Config.h/.cpp        All data structures and JSON serialisation
  Fermenter.h/.cpp     Temperature control loop (heating/cooling/hysteresis state machine)
  Temperatures.h/.cpp  DS18B20 probe management
  SmartPlugs.h/.cpp    RF smart plug control
  Profile.h/.cpp       Fermentation profile runner
  Reports.h/.cpp       Brewfather / Brewer's Friend reporting
  iSpindel.h/.cpp      iSpindel WiFi hydrometer receive and registration
  Tilt.h/.cpp          Tilt hydrometer via HM-10 BLE
  Mqtt.h/.cpp          MQTT client — publishing, HA discovery, command dispatch
  WebAPI.h/.cpp        REST API web server
  WebAdmin.cpp         Admin configuration page (PROGMEM HTML)
  Log.h/.cpp           Centralised serial & syslog logging with timestamps
  Pins.h               Hardware GPIO pin assignments
  Version.h            Firmware version constants
```

---

*All JSON field names, file paths, REST routes, and error strings are preserved
verbatim from the original firmware for compatibility with existing device configs.*
