/*
 * Copyright 2026 Sean Cardus
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
/*
 * UpdateCheck.h — Daily check for a newer firmware release
 *
 * Once a day the controller downloads UPDATE_CHECK_URL (see Version.h), a
 * tiny JSON file published by the ourbrewbot.com website:
 *
 *   {"version":"0.4.16","notes":"https://github.com/.../releases/tag/v0.4.16"}
 *
 * and compares "version" with the running FW_VERSION. The request carries the
 * chip ID and firmware version (?id=2924fa&v=0.4.16) so the site can count
 * the controllers in use. It only REPORTS the
 * result - in syslog, over MQTT (Home Assistant "update" entity) and on the
 * admin page. It never downloads or installs anything.
 *
 * The daily check can be switched off (GlobalConfig.updateCheck); a check
 * asked for from the admin page (POST /update/check) always runs.
 */

#include <Arduino.h>

struct UpdateStatus {
  bool     checked;            // true once any check has got a valid reply since boot
  bool     updateAvailable;    // the latest release is newer than this firmware
  char     latestVersion[16];  // e.g. "0.4.16" - empty until a check succeeds
  char     notesUrl[96];       // release notes link - may be empty
  bool     attempted;          // true once any check has been tried since boot
  uint32_t lastCheckMs;        // millis() of the last attempt (valid when attempted)
  char     lastError[40];      // why the last attempt failed - empty if it worked
};

extern UpdateStatus g_updateStatus;

// Split "1.2.3" (a leading "v" is allowed) into its three numbers.
// Returns false for anything else, e.g. "1.2", "1.2.3-beta" or "".
bool parseVersion(const char* text, uint16_t parts[3]);

// True only if both strings are valid versions and `candidate` is newer
// than `current`. Anything unparseable counts as "not newer".
bool isNewerVersion(const char* candidate, const char* current);

// Read version.json. Copies "version" and (if present and a web link)
// "notes" into the buffers. Returns false if the reply isn't usable.
bool parseVersionJson(const String& body,
                      char* version, size_t versionSize,
                      char* notes, size_t notesSize);

// Check now. Blocks the loop for up to ~5 s (the HTTP timeout). Updates
// g_updateStatus, logs the result and publishes it over MQTT.
// Returns true if the check got a valid reply.
bool runUpdateCheck();

// Call every loop pass: runs the scheduled check when it's due.
void updateCheckLoop();
