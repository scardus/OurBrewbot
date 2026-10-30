# OurBrewbot Firmware

Replacement firmware for the MyBrewbot ESP8266 fermentation controller.

---

## What this does

The original firmware depended on `mybrewbot.co.uk` for cloud storage, the Blynk App, and OTA updates. All of those are now dead.

This firmware keeps your hardware working:
- All temperature probe control preserved (DS18B20 probes, same addressing)
- Smart plug RF transmit preserved (same codes, same protocol)
- Fermentation profile runner preserved
- Brewfather / Brewer's Friend integrations with per-fermenter toggles
- Reads the original's config files and converts them on first start - see [Restoring Your Original Config](#restoring-your-original-config)
- Pressing the reset button twice clears the saved WiFi, as on the original - see [First Run](#first-run)
- iSpindel HTTP receiver
- OTA firmware update via web browser
- Tilt hydrometer support via HM-10 BLE module (standard and Tilt Pro)

New/Updated Features:
- Install from your browser at [ourbrewbot.com](https://ourbrewbot.com/install) — no tools needed
- Web-based admin page for configuring probes, fermenters, Tilts, iSpindels, plugs, and services
- Full local REST API
- mDNS — device registers as `ourbrewbot-CHIPID.local` on the local network
- Gravity source picked automatically - a fermenter's Tilt, then a reporting iSpindel, then an estimate from the OG, target gravity and time since the profile started. This replaces the original's SG Control setting, which only chose the source; it is kept in the config file but does nothing
- Tilt Pro support - auto-detected by broadcast value magnitude; gravity and temperature decoded with correct Pro precision (÷10000 / ÷10), labelled "Pro" in the admin UI
- Brewfather integration uses the Custom Stream API
- MQTT support for publishing fermenter data to any MQTT broker
- Home Assistant MQTT Discovery & Control - auto-creates HA entities with no YAML needed (optional, per-broker toggle)
- LittleFS file browser in admin page for inspecting config files
- BLE AT command console for debugging HM-10 Bluetooth module
- Live log viewer - the **Log** button on the admin page opens a drawer showing the controller's log as it happens, with no syslog server or MQTT broker needed. It starts with the last few seconds of lines and keeps collecting while the admin page is visible, even with the drawer closed. It is a live view, not a stored log: lines can be missed if the browser falls behind or the page is hidden, and the drawer says when that happens
- Rebuilt Fermentation Profiles tab — 4 editable profiles with up to 15 steps each, per-fermenter assignment, start/stop/pause, manual step navigation
- Daily firmware update check — tells you (admin page, syslog, Home Assistant) when a newer release is out
- Crash reports — after a crash or watchdog reset, the crash details are sent to ourbrewbot.com to help fix bugs

The update check and crash reports can each be switched off — see [Contacting ourbrewbot.com](#contacting-ourbrewbotcom).

Not yet implemented / tested:
- Pressure sensor - __Untested - No hardware__
- Aux 1 / Aux 2 smart plug roles - __Not implemented__ - from the original firmware; a plug can be given the role, but nothing switches it

---

## How this was done

The hardware connections were traced with a simple multimeter.  Analysis of the previous firmware image was performed by Claude Code.  Reverse engineering of the machine code was unfeasible, so instead we pulled generic information from the JSON configuration files and any function name strings/symbols it could find.  From this, it was possible to build a basic framework to iterate upon and re-build features.

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

In `platformio.ini`, set `upload_port` and `monitor_port` to match your device's COM port (currently `COM6`). Adjust if your port differs.

### 5. Build and upload

- **Build only:** click the checkmark (✓) in the PlatformIO toolbar, or run `PlatformIO: Build` from the command palette (`Ctrl+Alt+B`).
- **Build and upload:** click the right-arrow (→) in the toolbar, or run `PlatformIO: Upload` (`Ctrl+Alt+U`).
- **Serial monitor:** click the plug icon in the toolbar, or run `PlatformIO: Monitor`.

---

## Backing up your original firmware & settings

Before flashing, back up the full 4 MB flash from your existing MyBrewbot device so you can restore it, if needed.

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

Pre-built binaries are attached to each release on the [GitHub Releases](../../releases) page of this repository, named `OurBrewbot_vX.Y.Z.bin`. You do **not** need VS Code or PlatformIO installed — just the binary and one of the tools below.

Put the device in bootloader mode as described above before flashing.

### Option A — Web installer (easiest)

Open [ourbrewbot.com/install](https://ourbrewbot.com/install) in Chrome or Edge on a desktop computer, connect the device by USB and follow the steps. It always installs the latest release.

### Option B — esptool.py (cross-platform)

```bash
esptool.py --port COM7 --baud 115200 write_flash 0x0 OurBrewbot_vX.Y.Z.bin
```

> Replace `COM7` with your actual port and `OurBrewbot_vX.Y.Z.bin` with the filename downloaded from the Releases page.

### Option C — Espressif Flash Download Tool (Windows GUI)

1. Run the tool, select **ESP8266** / **Develop** / **UART**
2. Click the **SPIDownload** tab
3. Tick the checkbox on the first row, click `...` to browse to `OurBrewbot_vX.Y.Z.bin`, and set the address to `0x0`
4. Set **SPI Speed** to `40MHz` and **SPI Mode** to `DIO`
5. Select your COM port and baud rate to 115200
6. Click **START**

### Option D — OTA (no cable, after initial flash)

Once the firmware is running, navigate to `http://ourbrewbot-XXXXXX.local/update` in your browser and upload the `.bin` file directly.

Every smart plug is switched off when the upload starts and held off until the controller restarts on the new firmware. If the upload fails, control carries on as before, but a fridge that was cooling waits out its compressor delay before it runs again. Plugs are also switched off before a reboot or config reset.

---

## First Run

1. On first boot the device creates a WiFi access point named `OurBrewbot-XXXXXX` - __Make a note of this!__ It stays open until you have set up WiFi, and the fermenters are controlled meanwhile.
2. Connect to it from your phone or laptop
3. A configuration portal opens — enter your WiFi SSID and password
4. The device closes the access point, connects to your network and registers its name in mDNS
5. Open http://ourbrewbot-XXXXXX.local (the XXXXXX noted in step 1) in your browser.  If this does not work, find your device's IP from your router admin page, then open `http://DEVICEIP/` in a browser.

If your WiFi is down when the device starts, it carries on controlling the fermenters without it and keeps trying to reconnect. After 1 minute without WiFi it also opens the `OurBrewbot-XXXXXX` access point for 2 minutes at a time, taking turns with 1 minute of reconnecting, so you can enter new WiFi details if they have changed. While your phone is connected to the access point it stays open for up to 5 minutes.

To clear the saved WiFi, for example to move the controller to a different network, press the reset button twice, the second press within 10 seconds of the first. The `OurBrewbot-XXXXXX` access point then opens and stays open until you enter new WiFi details, and the fermenters are controlled meanwhile. Only the reset button counts: a crash, a reboot or a power cut never clears the WiFi. The admin page's **Reset WiFi Settings** button does the same.

---

## Restoring Your Original Config

The original firmware keeps its settings in the LittleFS partition, and they survive flashing OurBrewbot as long as the flash isn't erased. OurBrewbot uses the same file names and field names, but several settings mean something different, so on its first start it converts them once:

- **Fermenters** - the one(s) you were using are switched on and the rest off. Your floor and ceiling are widened by the hysteresis, so heating and cooling start at the same temperatures as before. The alarm tolerance is set to 3.0 °C (the original used a percentage). Settings saved in Fahrenheit are converted to Celsius.
- **Smart plugs** - each plug gets the job its slot had in the original (slot 1 = Fermenter 1 heating, slot 2 = Fermenter 1 cooling, and so on, slots 9 and 10 = Aux). Plugs set up as "Dial" plugs get the codes the original sent for them.
- **Probes, Tilts and iSpindels** - keep their fermenter and role. The one the original used for temperature control becomes that fermenter's beer temperature. Tilt calibrations are carried across.
- **Profiles** - steps are converted to OurBrewbot's step types. A profile that was running is stopped - start it again on the Profiles tab.
- **Brew service** - Brewfather or Brewer's Friend keeps its ID, and is switched on for the fermenters that sent to it. Press the service's **Test** button to check it still works.
- **WiFi** - your network name and password are moved into the controller's own WiFi store, so it joins your WiFi as before. The original's WiFi settings file, which held the password in plain text, is then deleted. If your WiFi has changed since, set it up through the `OurBrewbot-XXXXXX` access point as described above.

Not carried across: your probe names (OurBrewbot names probes by their position), Plaato, MyBrewBuddy, monitor.beer, and the Blynk token.

The untouched original files are kept in the `orig` folder, and `orig/conversion.txt` lists every change that was made. You can read both in the admin page's file browser on the **System Settings** tab. Check your plugs and fermenters there before you start a brew.

---

## Contacting ourbrewbot.com

The controller runs entirely on your own network. It only contacts ourbrewbot.com for two things, and both can be switched off in the admin page on the **System Settings** tab, under **Global Settings**:

| Setting | What it does | What is sent |
|---------|--------------|--------------|
| **Update Check** | Once a day, downloads `http://ourbrewbot.com/version.json` and compares it with the running version. The result is shown on the admin page, in syslog and as a Home Assistant `update` entity. Nothing is downloaded or installed. The **[check]** link next to the firmware version (`POST /update/check`) works even when the daily check is off. | The chip ID and firmware version (used to count the controllers in use) |
| **Crash Reports** | After the device restarts from a crash or watchdog reset, it saves a crash report and sends it to ourbrewbot.com once WiFi has been connected for about 2 minutes. Multiple crash reports will be saved in queue up to a maximum of 5. Switching Crash Reports off, or a factory reset, deletes any reports still waiting. | The chip ID, firmware version and build date, the reset reason, the last code area that ran, the processor registers (including those of the first CPU exception, if a second one followed), the top of the stack (raw memory, as hex numbers), how long the device had been running, its free memory, and its WiFi signal strength |

Neither sends your settings, WiFi network name or password, temperatures or brewing data. The stack snapshot is a few dozen raw memory words from the moment of the crash, used to find which bit of the code crashed. The running time, free memory and WiFi signal strength help tell a hardware or WiFi problem from a firmware bug.

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
| GET    | /controller        | Controller config + plugs     |
| POST   | /controller        | Update global config          |
| GET    | /debug             | Debug mode and per-fermenter sensor overrides |
| POST   | /debug             | Set debug mode / sensor overrides (runtime only, not saved) |
| GET    | /fermenter?id=0    | Single fermenter              |
| POST   | /fermenter         | Update fermenter config       |
| POST   | /fermenter/profile | Profile control (start/stop/pause/next/prev) |
| GET    | /fermenters        | All fermenter data (JSON)     |
| GET    | /fs/file           | Read LittleFS file content    |
| GET    | /fs/files          | List LittleFS files           |
| POST   | /fs/save           | Save a LittleFS config file   |
| GET    | /health            | System health                 |
| GET    | /logs?boot=&after= | Recent log lines as plain text, after a sequence number (used by the admin page's Log drawer) |
| POST   | /iSpindel          | iSpindel gravity data         |
| POST   | /ispindel/config   | Update iSpindel config (fermenter, collect data, clear) |
| GET    | /ispindels         | iSpindel config + live data   |
| GET    | /mqtt              | MQTT config (includes haDiscovery flag; password masked) |
| POST   | /mqtt              | Update MQTT config (haDiscovery, LWT, discovery cleanup on disable) |
| POST   | /mqtt/discover     | Trigger HA MQTT discovery     |
| POST   | /mqtt/test         | Test MQTT connection          |
| GET    | /probes            | All temperature probes        |
| POST   | /probes            | Update probe config           |
| POST   | /profile           | Update fermentation profile   |
| GET    | /profiles          | Fermentation profile config   |
| GET    | /reboot            | Asks to confirm a reboot (page with a button) |
| POST   | /reboot            | Reboot device                 |
| GET    | /reset             | Asks to confirm a factory reset (page with a button) |
| POST   | /reset?target=config | Reset all config to defaults |
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
| POST   | /update/check      | Check ourbrewbot.com for a newer firmware now |
| GET    | /WiFi              | WiFi config page (alias)      |
| POST   | /wifi/reset        | Clear WiFi settings and reboot into the setup portal |

The API has no login, so keep the controller on a network you trust. It does refuse a POST, firmware uploads included, that a web page from another website sends through your browser: a request whose `Origin` header names a different site gets a 403. The WebUI, curl, scripts, Home Assistant and iSpindels are not affected. The API also doesn't send an `Access-Control-Allow-Origin` header, so another website can't read its replies through your browser either.

---

## Home Assistant MQTT Integration

When MQTT is enabled and **HA Discovery** is turned on, the device publishes Home Assistant MQTT discovery payloads on connect and whenever HA restarts. No manual YAML configuration is needed — entities appear automatically in HA.

The broker password is never shown once it is saved: `GET /mqtt` returns `********` in its place, and the admin page's Password field reads "unchanged" — leave it empty to keep the saved password. It is still stored in `jsonMqtt.txt`, so the file browser and **Export Config** files contain it; keep exported files private.

**More than one controller on the same broker:** give each one its own **Base Topic** in the MQTT settings (for example `ourbrewbot-garage` and `ourbrewbot-shed`). Every controller starts with the base topic `ourbrewbot`. Two controllers with the same base topic publish over each other's readings and status, so Home Assistant shows a mix of the two, and with **Allow HA Control** on, both act on the same commands: switching Fermenter 1 on one switches Fermenter 1 on the other.

### Entity types

Discovery creates one HA device for the controller, one for each fermenter, and one for each configured probe, Tilt and iSpindel. Empty or unassigned slots are skipped.

| HA device | Entity type | Fields |
|-----------|-------------|--------|
| `OurBrewbot` (controller) | `sensor` | firmware_version, ip_address, mdns_name, wifi_ssid, rssi, free_heap, uptime, chip_id, reboot_reason, reboot_code (all diagnostic) |
| | `button` | reboot, all_off |
| | `update` | firmware (installed vs latest release, from the daily update check; display-only) |
| `OurBrewbot F0`–`F3` (fermenters) | `sensor` | beer_temperature, ambient_temperature, gravity, gravity_source, attenuation, status, beer_temperature_source, temperature_unit, profile_step, profile_steps |
| | `binary_sensor` | alarm |
| | `switch` | power, temp_control, profile_running |
| | `number` | ceiling_temperature, floor_temperature, hysteresis, compressor_delay, og, tg |
| | `text` | name, beer_name, yeast |
| | `select` | profile_no (options: 0–4) |
| `OurBrewbot Probe <name>` | `sensor` | temperature, name, function, fermenter |
| | `binary_sensor` | active |
| `OurBrewbot Tilt <colour>` | `sensor` | temperature, gravity, fermenter, function |
| | `binary_sensor` | active, is_pro |
| `OurBrewbot iSpindel <name>` | `sensor` | temperature, gravity, corrected_gravity, battery, rssi, angle, velocity, run_time, name, fermenter, function |

### Allow HA Control

By default the device is read-only from HA's perspective. Enable **Allow HA Control** in the MQTT settings page to allow Home Assistant to send commands back to the device.

When enabled, the device subscribes to `<baseTopic>/+/+/set` and accepts commands for all writable entity types (switch, number, text, select, button). Commands are validated — out-of-range values are silently rejected and the HA entity reverts to the actual device state within ~60 seconds via the regular state publish.

The **All Off** button switches every smart plug off and powers every fermenter off, so nothing comes back on until a fermenter's Power switch is turned on again.

The **profile_running** switch pauses a fermenter's profile when turned off and resumes it at the same step when turned on. Turned on with no profile running, it starts the selected profile. Stopping a profile is done from the WebUI.

When disabled, all discovery entities are still advertised (so the dashboard YAML remains stable), but the device ignores any incoming commands.

### Dashboard

A ready-made Lovelace dashboard is provided in `HomeAssistant/dashboard.yaml`. It requires these HACS frontend cards:
- [lovelace-plotly-graph-card](https://github.com/dbuezas/lovelace-plotly-graph-card)
- [lovelace-mushroom](https://github.com/piitaya/lovelace-mushroom)
- [card-mod](https://github.com/thomasloven/lovelace-card-mod)

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
  MqttParse.h/.cpp     MQTT command-topic parsing (split out so it can be unit tested)
  WebAPI.h/.cpp        REST API web server
  WebAdmin.cpp         Admin configuration page (PROGMEM HTML)
  Log.h/.cpp           Centralised serial & syslog logging with timestamps
  Crash.h/.cpp         Crash and watchdog-reset details saved across reboot, logged on the next boot
  FirstFault.h/.cpp    Keeps the first CPU exception of a crash, in case the crash handling faults too
  CrashReport.h/.cpp   Sends the saved crash details to ourbrewbot.com
  UpdateCheck.h/.cpp   Daily check of ourbrewbot.com for a newer firmware release
  OriginalConfig.h/.cpp  One-time conversion of an original MyBrewbot's settings
  DoubleReset.h/.cpp   Two presses of the reset button clear the saved WiFi
  Pins.h               Hardware GPIO pin assignments
  Version.h            Firmware version constants and ourbrewbot.com URLs
```

---

## Licence

OurBrewbot is licensed under the [Apache License, Version 2.0](LICENSE). See [NOTICE](NOTICE) for the copyright notice.

---

*JSON field names and file paths are kept from the original firmware, so its config files can be read and converted. The REST API is OurBrewbot's own and is not compatible with the original's.*
