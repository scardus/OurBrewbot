#pragma once
/*
 * Version.h — Firmware version constants
 */

#define FW_VERSION      "0.4.16"
#define FW_SWNO         20          // preserved for config compatibility
#define FW_BUILD_DATE   __DATE__ " " __TIME__

// Where the daily update check (UpdateCheck.cpp) reads the latest release.
#define UPDATE_CHECK_URL "http://ourbrewbot.com/version.json"
