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
- Full local REST API
- Web-based admin page for configuring probes, fermenters, Tilts, iSpindels, plugs, and services
- mDNS — device registers as `ourbrewbot-CHIPID.local` on the local network
- LittleFS file browser in admin page for inspecting config files
- BLE AT command console for debugging HM-10 Bluetooth module

Not yet implemented / tested:
- Pressure sensor - __Untested - No hardware__
- Fermentation Profiles tab — 4 editable profiles with up to 15 steps each, per-fermenter assignment, start/stop/pause, manual step navigation __Untested__
- Alarms - No mobile app or cloud service, so nothing to send alarms to. Web-hooks/alert providers may be a replacement?

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
| GET    | /fermenters        | All fermenter data (JSON)     |
| GET    | /fermenter?id=0    | Single fermenter              |
| POST   | /fermenter         | Update fermenter config       |
| GET    | /controller        | Controller config + plugs     |
| POST   | /controller        | Update global config          |
| GET    | /status            | Quick status all fermenters   |
| GET    | /probes            | All temperature probes        |
| POST   | /probes            | Update probe config           |
| GET    | /health            | System health                 |
| GET    | /smartplugs        | Smart plug config (JSON)      |
| POST   | /smartplug         | Update smart plug config      |
| POST   | /smartplug/test    | Test smart plug RF on/off     |
| GET    | /rf/sniff          | RF sniff page                 |
| GET    | /rf/sniff/poll     | Poll RF sniff results         |
| GET    | /ble/sniff         | BLE AT command console page   |
| GET    | /ble/sniff/poll    | Poll BLE serial data          |
| POST   | /ble/sniff/send    | Send AT command to HM-10      |
| GET    | /brewservices      | Brew service config           |
| POST   | /brewservices      | Update brew service config    |
| POST   | /brewservices/test | Test brew service connection  |
| GET    | /mqtt              | MQTT config (includes haDiscovery flag) |
| POST   | /mqtt              | Update MQTT config (haDiscovery, LWT, discovery cleanup on disable) |
| POST   | /mqtt/test         | Test MQTT connection          |
| POST   | /mqtt/discover     | Trigger HA MQTT discovery     |
| GET    | /profiles          | Fermentation profile config   |
| POST   | /profile           | Update fermentation profile   |
| POST   | /fermenter/profile | Profile control (start/stop/pause/next/prev) |
| GET    | /board_info.json   | Board info                    |
| POST   | /iSpindel          | iSpindel gravity data         |
| GET    | /config            | WiFi config page              |
| GET    | /configMe          | Save WiFi config (form GET)   |
| GET    | /WiFi              | WiFi config page (alias)      |
| GET    | /update            | OTA firmware update page      |
| POST   | /update            | Upload new firmware binary    |
| GET    | /reset             | Reset all config to defaults  |
| GET    | /reboot            | Reboot device                 |
| GET    | /fs/files          | List LittleFS files           |
| GET    | /fs/file           | Read LittleFS file content    |
| GET    | /tilts             | Tilt hydrometer config + live data |
| POST   | /tilt              | Update Tilt config (fermenter, function, SG/temp adjust) |
| GET    | /syslog            | Syslog config |
| POST   | /syslog            | Update syslog config (host, port, facility, minLevel) |

---

## File Structure

```
OurBrewbot/
  OurBrewbot.ino       Main sketch — setup, loop, state machine
  Config.h/.cpp        All data structures and JSON serialisation
  Pins.h               Hardware GPIO pin assignments
  Version.h            Firmware version constants
  Log.h/.cpp           Serial logging routines
  Fermenter.h/.cpp     Temperature control loop (heating/cooling)
  Temperatures.h/.cpp  DS18B20 probe management
  SmartPlugs.h/.cpp    RF smart plug control
  Profile.h/.cpp       Fermentation profile runner
  Reports.h/.cpp       Brewfather / Brewer's Friend reporting
  iSpindel.h/.cpp      iSpindel WiFi hydrometer receive and registration
  Mqtt.h/.cpp          MQTT client for fermenter data publishing
  WebAPI.h/.cpp        REST API web server
  WebAdmin.cpp         Admin configuration page (PROGMEM HTML)
  Tilt.h/.cpp          Tilt hydrometer via HM-10 BLE
  Log.h/.cpp           Centralised serial logging with timestamps
```

---

*All JSON field names, file paths, REST routes, and error strings are preserved
verbatim from the original firmware for compatibility with existing device configs.*
