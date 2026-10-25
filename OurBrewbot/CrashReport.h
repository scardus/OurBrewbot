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
 * g_lastCrash at boot. This module saves it straight away, as one line of
 * JSON, to the crash report queue (FILE_CRASH_QUEUE), then POSTs each saved
 * report to CRASH_REPORT_URL (see Version.h), oldest first:
 *
 *   {"id":"2924fa","v":"0.4.16","build":"Sep 27 2026 09:29:50","reset":2,
 *    "last":"MQTT_PEND","reason":2,"exccause":28,"epc1":"4021a3b0",...,
 *    "stack":["40201234",...],"stack_free":924,"stack_at":"WEB /iSpindel"}
 *
 * stack_free is the least loop stack left at any point before the crash, and
 * stack_at the subsystem (and web URL) that took it there - see StackCheck.cpp.
 *
 * A hardware watchdog report has only id, v, build, reset and last - the
 * crash handler never runs for one, so there are no registers or stack.
 *
 * The queue is a file so that reports survive until WiFi is available - even
 * through more crashes and reboots. It holds at most 5; saving a sixth deletes
 * the oldest. Sending starts once WiFi has been up for 2 minutes, with 30
 * seconds between reports. A failed send is retried every 10 minutes, up to 4
 * tries per boot; after that the reports wait for the next boot. Switched off
 * by GlobalConfig.crashReports, which also deletes any reports still waiting.
 */

#include <Arduino.h>
#include "Crash.h"

// Build the JSON report for `crash` into `out`. Returns false if it doesn't fit.
bool buildCrashReportJson(const CrashInfo& crash, char* out, size_t outSize);

// Call every loop pass: saves this boot's crash (if any) to the queue, then
// sends the saved reports when they are due. Does nothing once the queue is
// empty.
void crashReportLoop();
