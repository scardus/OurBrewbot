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

New/Updated Features:
- Brewfather integration uses the Custom Stream API
- MQTT support for publishing fermenter data to any MQTT broker
- Home Assistant MQTT Discovery — auto-creates HA entities with no YAML needed (optional, per-broker toggle)
- Full local REST API
- Web-based admin page for configuring probes, fermenters, plugs, and services
- Fermentation Profiles tab — 4 editable profiles with up to 15 steps each, per-fermenter assignment, start/stop/pause, manual step navigation
- mDNS — device registers as `ourbrewbot-CHIPID.local` on the local network
- LittleFS file browser in admin page for inspecting config files
- BLE AT command console for debugging HM-10 Bluetooth module
- Tilt hydrometer support via HM-10 BLE module — iBeacon scanning every 30s with auto-discovery, per-colour fermenter assignment, and SG/temperature calibration offsets
- Detailed reboot reason logging on startup — logs reset cause string and numeric code to serial; on exception crashes also logs cause register, EPC1, and EXCVADDR; persists numeric code and exception registers to the reboot log file

Not yet implemented / tested:
- iSpindel HTTP receiver  
- OTA firmware update via web browser
- Black temperature probe jack is currently disabled, due to it causing crashes on boot

Removed:
- mybrewbot.co.uk cloud backend (server gone)
- Blynk dashboard (replaced with REST API + admin page)

---

## How this was done

The hardware connections were traced with a simple multimeter.  Analysis of the previous firmware image was performed by Claude Code.  Reverse engineering of the machine code was unfeasable, so instead we pulled generic information from the JSON configuration files and any function name strings/symbols it could find.  From this, it was possible to build a basic framework to iterate upon and re-build features.

---

## Hardware

Pin assignments are in `Pins.h` using raw GPIO numbers (compatible with both
NodeMCU and generic ESP8266 board targets). Bus 2 is currently disabled by default
because GPIO9/SD2 conflicts with the SPI flash on ESP-12E/F modules, causing
a crash on boot.

| Function       | GPIO  | NodeMCU Pin | Label          |
|----------------|-------|-------------|----------------|
| Probe Bus 1    | GPIO0 | D3          | Green Jack     |
| Probe Bus 2    | GPIO9 | SD2         | Black Jack (disabled by default) |
| RF Transmitter | GPIO4 | D2          | FS1000A TX     |
| RF Receiver    | GPIO4 | D5          | MX-RM-5V RX    |
| BLE (ESP TX)   | GPIO12| D6          | HM-10 BT 4.0 RX ← ESP TX |
| BLE (ESP RX)   | GPIO13| D7          | HM-10 BT 4.0 TX → ESP RX |


---

## Building

### 1. Install Arduino IDE

Download from https://www.arduino.cc/en/software

### 2. Install ESP8266 board support

In Arduino IDE: File → Preferences → Additional Boards Manager URLs, add:
```
http://arduino.esp8266.com/stable/package_esp8266com_index.json
```
Then: Tools → Board → Boards Manager → search "esp8266" → Install

### 3. Install libraries

Tools → Manage Libraries, install each:
- **ArduinoJson** by Benoit Blanchon (v6.x)
- **DallasTemperature** by Miles Burton
- **OneWire** by Jim Studt
- **WiFiManager** by tzapu
- **rc-switch** by sui77
- **PubSubClient** by Nick O'Leary
- **SoftwareSerial** (included with ESP8266 core)

### 4. Board settings

- Board: **Generic ESP8266 Module** (or NodeMCU/Wemos D1 Mini — pins use raw GPIO numbers)
- Flash Mode: **DIO**
- Flash Size: **4MB (FS:2MB OTA:~1019KB)**
- Crystal Frequency: 80 MHz
- Reset Method: NodeMCU
- Upload Speed: 115200

### 5. Open and upload

Open `OurBrewbot.ino` from the `OurBrewbot/` folder, select your port, click Upload.

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
  Mqtt.h/.cpp          MQTT client for fermenter data publishing
  WebAPI.h/.cpp        REST API web server
  WebAdmin.cpp         Admin configuration page (PROGMEM HTML)
  Tilt.h/.cpp          Tilt hydrometer via HM-10 BLE
  Log.h/.cpp           Centralised serial logging with timestamps
```

---

*All JSON field names, file paths, REST routes, and error strings are preserved
verbatim from the original firmware for compatibility with existing device configs.*
