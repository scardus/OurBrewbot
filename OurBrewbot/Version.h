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
 * Version.h — Firmware version constants
 */

#define FW_VERSION      "0.6.0"
#define FW_SWNO         20          // preserved for config compatibility
#define FW_BUILD_DATE   __DATE__ " " __TIME__

// Where the daily update check (UpdateCheck.cpp) reads the latest release.
#define UPDATE_CHECK_URL "http://ourbrewbot.com/version.json"

// Where a crash report is sent (CrashReport.cpp) after a crash or watchdog reset.
#define CRASH_REPORT_URL "http://ourbrewbot.com/api/crash"
