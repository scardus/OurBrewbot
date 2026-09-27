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
 * CrashReport.h — Send a crash report to ourbrewbot.com after a crash
 *
 * After a crash or watchdog reset, Crash.cpp copies what it found into
 * g_lastCrash at boot. About 2 minutes later this module POSTs it once, as
 * JSON, to CRASH_REPORT_URL (see Version.h):
 *
 *   {"id":"2924fa","v":"0.4.16","build":"Sep 27 2026 09:29:50","reset":2,
 *    "last":"MQTT_PEND","reason":2,"exccause":28,"epc1":"4021a3b0",...,
 *    "stack":["40201234",...]}
 *
 * A hardware watchdog report has only id, v, build, reset and last - the
 * crash handler never runs for one, so there are no registers or stack.
 *
 * A failed send is retried up to 3 times, 10 minutes apart. The report is
 * only kept in RAM, so a reboot before it's sent loses it. Switched off by
 * GlobalConfig.crashReports.
 */

#include <Arduino.h>
#include "Crash.h"

// Build the JSON report for `crash` into `out`. Returns false if it doesn't fit.
bool buildCrashReportJson(const CrashInfo& crash, char* out, size_t outSize);

// Call every loop pass: sends the report once it's due, and does nothing
// after a normal boot.
void crashReportLoop();
