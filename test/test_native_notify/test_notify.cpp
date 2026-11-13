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

// Native (host) unit tests for the push notifications in OurBrewbot/Notify.cpp:
// the text of every message, the queue, and what happens when a send fails.
//
// Notify.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test. The network is replaced by
// test/stubs/ESP8266HTTPClient.h, which records the URL, headers and body of
// each POST instead of sending it.

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#include "../../OurBrewbot/Config.h"
#include "../../OurBrewbot/Crash.h"
#include "../../OurBrewbot/UpdateCheck.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
FermenterConfig g_fermenters[MAX_FERMENTERS];
ProbeConfig     g_probes[MAX_PROBES];
iSpindelConfig  g_iSpindels[MAX_ISPINDELS];
TiltConfig      g_tilts[MAX_TILTS];
GlobalConfig    g_globalConfig;
NotifyConfig    g_notifyConfig;
UpdateStatus    g_updateStatus;

// ---- the uptime clock (Uptime.cpp), settable per test ----
static uint32_t s_uptimeSec = 0;
uint32_t uptimeSeconds() { return s_uptimeSec; }
uint32_t uptimeStamp()   { return s_uptimeSec ? s_uptimeSec : 1; }
uint32_t uptimeSince(uint32_t stamp) { return (s_uptimeSec > stamp) ? s_uptimeSec - stamp : 0; }

// ---- logMsg(): the last line is kept so tests can check what was logged ----
static char s_lastLog[200];
static int  s_logCalls = 0;
void logMsgImpl(uint8_t, PGM_P fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(s_lastLog, sizeof(s_lastLog), fmt, args);
  va_end(args);
  s_logCalls++;
}

// ---- small stand-ins for other modules ----
static uint8_t s_lastCheckpoint = 0xFF;
void checkpoint(uint8_t module) { s_lastCheckpoint = module; }

// This boot's id (Log.cpp), settable so a test can play a restart
static uint32_t s_bootId = 0x0BADF00D;
uint32_t logBootId() { return s_bootId; }

// The same conversion as Temperatures.cpp
float toDisplayTemp(float celsius) {
  return (g_globalConfig.unit == UNIT_CELSIUS) ? celsius : celsius * 9.0f / 5.0f + 32.0f;
}

static const char* const TILT_COLOUR_NAMES[] = {
  "Red", "Green", "Black", "Purple", "Orange", "Blue", "Yellow", "Pink"
};
const char* getTiltColourName(uint8_t colour) {
  return (colour < 8) ? TILT_COLOUR_NAMES[colour] : "Unknown";
}

// The code under test
#include "../../OurBrewbot/Notify.cpp"

// ---- test fixture ----

static NotifyMessage msg;

void setUp(void) {
  for (int i = 0; i < MAX_FERMENTERS; i++) g_fermenters[i] = FermenterConfig{};
  for (int i = 0; i < MAX_PROBES; i++)     g_probes[i]     = ProbeConfig{};
  for (int i = 0; i < MAX_ISPINDELS; i++)  g_iSpindels[i]  = iSpindelConfig{};
  for (int i = 0; i < MAX_TILTS; i++)      g_tilts[i]      = TiltConfig{};
  g_globalConfig = GlobalConfig{};
  g_globalConfig.unit = UNIT_CELSIUS;
  memset(&g_updateStatus, 0, sizeof(g_updateStatus));

  g_notifyConfig.enabled = true;
  strlcpy(g_notifyConfig.url, "http://ntfy.sh/my-topic", sizeof(g_notifyConfig.url));
  g_notifyConfig.events  = NOTIFY_EVT_ALL;

  strlcpy(g_fermenters[0].fermenterName, "Fermenter 1", sizeof(g_fermenters[0].fermenterName));
  strlcpy(g_fermenters[1].fermenterName, "Lager fridge", sizeof(g_fermenters[1].fermenterName));
  strlcpy(g_probes[2].probeName,   "Beer Temp", sizeof(g_probes[2].probeName));
  strlcpy(g_iSpindels[1].name,     "ispindel-4", sizeof(g_iSpindels[1].name));

  // Notify.cpp's queue is file-static (reachable because it's #included)
  s_queueCount = 0;
  s_tries      = 0;
  s_lastTryAt  = 0;
  memset(s_problemSent, 0, sizeof(s_problemSent));
  memset(s_problemSince, 0, sizeof(s_problemSince));
  memset(s_problemOffSince, 0, sizeof(s_problemOffSince));
  memset(s_problemMinutes, 0, sizeof(s_problemMinutes));
  memset(s_alarmReading, 0, sizeof(s_alarmReading));
  memset(s_profileFinishedAt, 0, sizeof(s_profileFinishedAt));
  s_restartCode = NO_RESTART;
  s_bootId      = 0x0BADF00D;

  WiFi.connected   = true;
  httpTestReset();
  s_uptimeSec      = 1000;
  s_lastLog[0]     = '\0';
  s_logCalls       = 0;
  s_lastCheckpoint = 0xFF;
  memset(&msg, 0, sizeof(msg));
}

void tearDown(void) {}

// Build the message for one event into `msg`
static bool build(NotifyEvent type, uint8_t index = 0, uint8_t detail = 0,
                  float a = 0.0f, float b = 0.0f) {
  NotifyEventData event = { type, index, detail, a, b };
  return notifyBuildMessage(event, msg);
}

// ============================================================
// MESSAGE TEXT
// ============================================================

void test_over_temperature_alarm_message(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_OVER, 0, 0, 29.24f, 29.0f));
  TEST_ASSERT_EQUAL_STRING("Temperature alarm", msg.title);
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 29.24 °C is above the 29.0 °C limit.", msg.body);
  TEST_ASSERT_EQUAL_STRING("warning", msg.tags);
  TEST_ASSERT_EQUAL_UINT8(4, msg.priority);
}

void test_under_temperature_alarm_message(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_UNDER, 1, 0, 8.5f, 10.0f));
  TEST_ASSERT_EQUAL_STRING("Temperature alarm", msg.title);
  TEST_ASSERT_EQUAL_STRING("Lager fridge: 8.50 °C is below the 10.0 °C limit.", msg.body);
}

void test_severe_alarm_is_urgent_and_says_so(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_OVER, 0, 1, 32.0f, 29.0f));
  TEST_ASSERT_EQUAL_STRING("Severe temperature alarm", msg.title);
  TEST_ASSERT_EQUAL_UINT8(5, msg.priority);
}

// Seen on the desk controller: a reading of 29.05 against a 29.0 limit was
// sent as "29.0 C is above the 29.0 C limit" when both had one decimal place
void test_a_reading_just_past_its_limit_does_not_look_equal_to_it(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_OVER, 0, 0, 29.05f, 29.0f));
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 29.05 °C is above the 29.0 °C limit.", msg.body);
}

void test_alarm_message_uses_fahrenheit_when_the_controller_does(void) {
  g_globalConfig.unit = 2;   // Fahrenheit
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_OVER, 0, 0, 30.0f, 25.0f));
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 86.00 °F is above the 77.0 °F limit.", msg.body);
}

void test_alarm_ended_message_gives_the_temperature(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_ENDED, 0, 0, 20.1f));
  TEST_ASSERT_EQUAL_STRING("Temperature alarm ended", msg.title);
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: back within its limits at 20.10 °C.", msg.body);
  TEST_ASSERT_EQUAL_STRING("white_check_mark", msg.tags);
  TEST_ASSERT_EQUAL_UINT8(3, msg.priority);
}

void test_alarm_ended_without_a_reading_says_so(void) {
  // The alarm also ends when the fermenter is switched off or its probe is lost
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_ENDED, 0, 0, TEMP_NONE));
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: the alarm has ended (no temperature reading).", msg.body);
}

void test_probe_messages(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_PROBE_LOST, 2));
  TEST_ASSERT_EQUAL_STRING("Temperature probe not responding", msg.title);
  TEST_ASSERT_EQUAL_STRING("Beer Temp is not responding. Its readings are no longer used.", msg.body);
  TEST_ASSERT_EQUAL_UINT8(4, msg.priority);

  TEST_ASSERT_TRUE(build(NOTIFY_PROBE_BACK, 2));
  TEST_ASSERT_EQUAL_STRING("Temperature probe responding again", msg.title);
  TEST_ASSERT_EQUAL_STRING("Beer Temp is responding again. Its readings are used.", msg.body);
}

void test_ispindel_messages(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_ISPINDEL_LOST, 1, 0, 45.0f));
  TEST_ASSERT_EQUAL_STRING("iSpindel stopped reporting", msg.title);
  TEST_ASSERT_EQUAL_STRING("ispindel-4: no report for 45 min. Its readings are no longer used.", msg.body);

  TEST_ASSERT_TRUE(build(NOTIFY_ISPINDEL_BACK, 1));
  TEST_ASSERT_EQUAL_STRING("iSpindel reporting again", msg.title);
  TEST_ASSERT_EQUAL_STRING("ispindel-4 is reporting again. Its readings are used.", msg.body);
}

void test_tilt_messages(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_TILT_LOST, 3, 0, 30.0f));
  TEST_ASSERT_EQUAL_STRING("Tilt stopped reporting", msg.title);
  TEST_ASSERT_EQUAL_STRING("Purple Tilt: no reading for 30 min. Its readings are no longer used.", msg.body);

  TEST_ASSERT_TRUE(build(NOTIFY_TILT_BACK, 3));
  TEST_ASSERT_EQUAL_STRING("Tilt reporting again", msg.title);
  TEST_ASSERT_EQUAL_STRING("Purple Tilt is reporting again. Its readings are used.", msg.body);
}

void test_restart_message_names_the_cause(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_RESTART, 0, REASON_EXCEPTION_RST));
  TEST_ASSERT_EQUAL_STRING("Controller restarted", msg.title);
  TEST_ASSERT_EQUAL_STRING("The controller restarted unexpectedly. Cause: crash.", msg.body);
  TEST_ASSERT_EQUAL_UINT8(4, msg.priority);

  TEST_ASSERT_TRUE(build(NOTIFY_RESTART, 0, REASON_DEFAULT_RST));
  TEST_ASSERT_EQUAL_STRING("The controller restarted unexpectedly. Cause: power on.", msg.body);
  TEST_ASSERT_TRUE(build(NOTIFY_RESTART, 0, REASON_WDT_RST));
  TEST_ASSERT_EQUAL_STRING("The controller restarted unexpectedly. Cause: hardware watchdog.", msg.body);
  TEST_ASSERT_TRUE(build(NOTIFY_RESTART, 0, REASON_SOFT_WDT_RST));
  TEST_ASSERT_EQUAL_STRING("The controller restarted unexpectedly. Cause: software watchdog.", msg.body);
  // A failed self-check restarts with the "software restart" code
  TEST_ASSERT_TRUE(build(NOTIFY_RESTART, 0, REASON_SOFT_RESTART));
  TEST_ASSERT_EQUAL_STRING("The controller restarted unexpectedly. Cause: crash.", msg.body);
  TEST_ASSERT_TRUE(build(NOTIFY_RESTART, 0, 99));
  TEST_ASSERT_EQUAL_STRING("The controller restarted unexpectedly. Cause: unknown.", msg.body);
}

void test_profile_messages(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_STEP_COMPLETE, 0, 2, 3.0f));
  TEST_ASSERT_EQUAL_STRING("Profile step complete", msg.title);
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: step 2 is complete. Step 3 has started.", msg.body);

  TEST_ASSERT_TRUE(build(NOTIFY_PROFILE_FINISHED, 1));
  TEST_ASSERT_EQUAL_STRING("Profile finished", msg.title);
  TEST_ASSERT_EQUAL_STRING("Lager fridge: the profile has finished.", msg.body);
}

void test_update_message_names_both_versions(void) {
  strlcpy(g_updateStatus.latestVersion, "9.9.9", sizeof(g_updateStatus.latestVersion));
  TEST_ASSERT_TRUE(build(NOTIFY_UPDATE_AVAILABLE));
  TEST_ASSERT_EQUAL_STRING("Firmware update available", msg.title);
  TEST_ASSERT_EQUAL_STRING("OurBrewbot 9.9.9 is available. This controller runs " FW_VERSION ".", msg.body);
  TEST_ASSERT_EQUAL_UINT8(2, msg.priority);
}

void test_test_message(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_TEST));
  TEST_ASSERT_EQUAL_STRING("OurBrewbot test", msg.title);
  TEST_ASSERT_EQUAL_STRING("Test notification from your OurBrewbot controller.", msg.body);
}

void test_an_empty_name_is_replaced_by_a_number(void) {
  g_fermenters[1].fermenterName[0] = '\0';
  TEST_ASSERT_TRUE(build(NOTIFY_PROFILE_FINISHED, 1));
  TEST_ASSERT_EQUAL_STRING("Fermenter 2: the profile has finished.", msg.body);

  g_probes[2].probeName[0] = '\0';
  TEST_ASSERT_TRUE(build(NOTIFY_PROBE_BACK, 2));
  TEST_ASSERT_EQUAL_STRING("Probe 3 is responding again. Its readings are used.", msg.body);

  g_iSpindels[1].name[0] = '\0';
  TEST_ASSERT_TRUE(build(NOTIFY_ISPINDEL_BACK, 1));
  TEST_ASSERT_EQUAL_STRING("iSpindel 2 is reporting again. Its readings are used.", msg.body);
}

void test_an_event_for_something_that_does_not_exist_builds_nothing(void) {
  TEST_ASSERT_FALSE(build(NOTIFY_ALARM_OVER,       MAX_FERMENTERS));
  TEST_ASSERT_FALSE(build(NOTIFY_ALARM_ENDED,      MAX_FERMENTERS));
  TEST_ASSERT_FALSE(build(NOTIFY_PROBE_LOST,       MAX_PROBES));
  TEST_ASSERT_FALSE(build(NOTIFY_PROBE_BACK,       MAX_PROBES));
  TEST_ASSERT_FALSE(build(NOTIFY_ISPINDEL_LOST,    MAX_ISPINDELS));
  TEST_ASSERT_FALSE(build(NOTIFY_ISPINDEL_BACK,    MAX_ISPINDELS));
  TEST_ASSERT_FALSE(build(NOTIFY_TILT_LOST,        MAX_TILTS));
  TEST_ASSERT_FALSE(build(NOTIFY_TILT_BACK,        MAX_TILTS));
  TEST_ASSERT_FALSE(build(NOTIFY_STEP_COMPLETE,    MAX_FERMENTERS));
  TEST_ASSERT_FALSE(build(NOTIFY_PROFILE_FINISHED, MAX_FERMENTERS));
}

// Every message with the longest names the settings can hold and the widest
// numbers the code lets through. If NOTIFY_BODY_SIZE is made too small, or a
// message is reworded past it, notifyBuildMessage() returns false and this
// fails - a notification must never be sent cut short.
void test_worst_case_messages_all_fit_the_buffer(void) {
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    memset(g_fermenters[i].fermenterName, 'W', sizeof(g_fermenters[i].fermenterName) - 1);
  }
  for (int i = 0; i < MAX_PROBES; i++) {
    memset(g_probes[i].probeName, 'W', sizeof(g_probes[i].probeName) - 1);
  }
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    memset(g_iSpindels[i].name, 'W', sizeof(g_iSpindels[i].name) - 1);
  }
  memset(g_updateStatus.latestVersion, '9', sizeof(g_updateStatus.latestVersion) - 1);
  g_globalConfig.unit = 2;   // Fahrenheit readings are the wider ones

  const float huge = 3.0e38f;   // far outside any real reading; shown as 999.9
  size_t longest = 0;
  for (int type = NOTIFY_ALARM_OVER; type <= NOTIFY_TEST; type++) {
    TEST_ASSERT_TRUE_MESSAGE(build((NotifyEvent)type, 0, 255, -huge, -huge), "negative extreme");
    if (strlen(msg.body) > longest) longest = strlen(msg.body);
    TEST_ASSERT_TRUE_MESSAGE(build((NotifyEvent)type, 0, 255, huge, huge), "positive extreme");
    if (strlen(msg.body) > longest) longest = strlen(msg.body);
  }
  // The longest today: an iSpindel that stopped reporting, 82 characters.
  // The titles and tags have fixed-size fields too: the longest title is
  // "Temperature probe responding again" (34), the longest tag
  // "white_check_mark" (16) - see test_headings_fit_their_fields.
  TEST_ASSERT_EQUAL_UINT(82, longest);
  TEST_ASSERT_TRUE(longest < NOTIFY_BODY_SIZE);
}

// setHeading() cuts a title or tag that is too long for its field, so check
// the two longest arrive whole
void test_headings_fit_their_fields(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_PROBE_BACK, 2));
  TEST_ASSERT_EQUAL_STRING("Temperature probe responding again", msg.title);
  TEST_ASSERT_EQUAL_STRING("white_check_mark", msg.tags);
  TEST_ASSERT_TRUE(strlen(msg.title) < sizeof(msg.title));
  TEST_ASSERT_TRUE(strlen(msg.tags) < sizeof(msg.tags));
}

void test_extreme_numbers_are_kept_to_a_fixed_width(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_OVER, 0, 0, 3.0e38f, -3.0e38f));
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 999.90 °C is above the -999.9 °C limit.", msg.body);
  TEST_ASSERT_TRUE(build(NOTIFY_ISPINDEL_LOST, 1, 0, 3.0e38f));
  TEST_ASSERT_EQUAL_STRING("ispindel-4: no report for 99999 min. Its readings are no longer used.", msg.body);
}

// ============================================================
// QUEUEING
// ============================================================

void test_nothing_is_queued_when_notifications_are_off(void) {
  g_notifyConfig.enabled = false;
  notifyEvent(NOTIFY_ALARM_OVER, 0, 0, 30.0f, 25.0f);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
}

void test_nothing_is_queued_for_a_kind_of_event_that_is_not_ticked(void) {
  g_notifyConfig.events = NOTIFY_EVT_ALARMS;
  notifyEvent(NOTIFY_PROBE_LOST, 2, 0, 5.0f);
  notifyEvent(NOTIFY_RESTART, 0, REASON_EXCEPTION_RST);
  notifyEvent(NOTIFY_PROFILE_FINISHED, 0);
  notifyEvent(NOTIFY_UPDATE_AVAILABLE);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
  notifyEvent(NOTIFY_ALARM_ENDED, 0, 0, 20.0f);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
}

void test_each_kind_of_event_follows_its_own_tick_box(void) {
  struct { NotifyEvent type; uint8_t bit; } rows[] = {
    { NOTIFY_ALARM_OVER,       NOTIFY_EVT_ALARMS   },
    { NOTIFY_ALARM_UNDER,      NOTIFY_EVT_ALARMS   },
    { NOTIFY_ALARM_ENDED,      NOTIFY_EVT_ALARMS   },
    { NOTIFY_PROBE_LOST,       NOTIFY_EVT_SENSORS  },
    { NOTIFY_PROBE_BACK,       NOTIFY_EVT_SENSORS  },
    { NOTIFY_ISPINDEL_LOST,    NOTIFY_EVT_SENSORS  },
    { NOTIFY_ISPINDEL_BACK,    NOTIFY_EVT_SENSORS  },
    { NOTIFY_TILT_LOST,        NOTIFY_EVT_SENSORS  },
    { NOTIFY_TILT_BACK,        NOTIFY_EVT_SENSORS  },
    { NOTIFY_RESTART,          NOTIFY_EVT_RESTARTS },
    { NOTIFY_STEP_COMPLETE,    NOTIFY_EVT_PROFILE  },
    { NOTIFY_PROFILE_FINISHED, NOTIFY_EVT_PROFILE  },
    { NOTIFY_UPDATE_AVAILABLE, NOTIFY_EVT_UPDATES  },
  };
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    s_queueCount = 0;
    g_notifyConfig.events = rows[r].bit;
    notifyEvent(rows[r].type);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1, s_queueCount, "queued when its box is ticked");

    s_queueCount = 0;
    g_notifyConfig.events = NOTIFY_EVT_ALL & ~rows[r].bit;
    notifyEvent(rows[r].type);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, s_queueCount, "not queued when only the others are");
  }
}

void test_the_test_message_is_never_queued(void) {
  notifyEvent(NOTIFY_TEST);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
}

void test_a_full_queue_drops_the_new_event_and_logs_it(void) {
  for (int n = 0; n < NOTIFY_QUEUE_SIZE; n++) notifyEvent(NOTIFY_PROFILE_FINISHED, 0);
  TEST_ASSERT_EQUAL_UINT8(NOTIFY_QUEUE_SIZE, s_queueCount);
  TEST_ASSERT_EQUAL_INT(0, s_logCalls);

  notifyEvent(NOTIFY_ALARM_OVER, 1, 0, 30.0f, 25.0f);
  TEST_ASSERT_EQUAL_UINT8(NOTIFY_QUEUE_SIZE, s_queueCount);
  TEST_ASSERT_EQUAL_INT(1, s_logCalls);
  // The events already waiting are untouched
  TEST_ASSERT_EQUAL_INT(NOTIFY_PROFILE_FINISHED, s_queue[NOTIFY_QUEUE_SIZE - 1].event.type);
}

// ============================================================
// TEMPERATURE ALARMS
// ============================================================

// One alarm check: the fermenter is over its 22.0 C ceiling, or back in band
static void alarmOn(uint8_t i, bool severe = false)  { notifyAlarmState(i, true, true, severe, 23.0f, 22.0f); }
static void alarmOff(uint8_t i)                      { notifyAlarmState(i, false, false, false, 21.0f, 22.0f); }

void test_an_alarm_is_reported_once_when_it_starts(void) {
  alarmOff(0);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);   // no alarm, nothing to say

  alarmOn(0);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_OVER, s_queue[0].event.type);
  TEST_ASSERT_EQUAL_UINT8(0, s_queue[0].event.index);
  TEST_ASSERT_EQUAL_UINT8(0, s_queue[0].event.detail);   // not severe
  TEST_ASSERT_EQUAL_FLOAT(23.0f, s_queue[0].event.a);
  TEST_ASSERT_EQUAL_FLOAT(22.0f, s_queue[0].event.b);

  // The alarm check runs every few seconds: it must not repeat the message
  for (int n = 0; n < 100; n++) { s_uptimeSec += 5; alarmOn(0); }
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
}

void test_an_under_temperature_alarm_is_reported_as_one(void) {
  notifyAlarmState(1, true, false, true, 9.0f, 18.0f);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_UNDER, s_queue[0].event.type);
  TEST_ASSERT_EQUAL_UINT8(1, s_queue[0].event.index);
  TEST_ASSERT_EQUAL_UINT8(1, s_queue[0].event.detail);   // severe
}

void test_a_mild_alarm_that_turns_severe_is_reported_again(void) {
  alarmOn(0);
  alarmOn(0, true);
  TEST_ASSERT_EQUAL_UINT8(2, s_queueCount);
  TEST_ASSERT_EQUAL_UINT8(1, s_queue[1].event.detail);
  // ...but only once, and not again when it eases back to mild
  alarmOn(0, true);
  alarmOn(0);
  TEST_ASSERT_EQUAL_UINT8(2, s_queueCount);
}

void test_alarm_ended_waits_until_the_alarm_has_been_off_ten_minutes(void) {
  alarmOn(0);
  s_queueCount = 0;

  alarmOff(0);
  s_uptimeSec += NOTIFY_PROBLEM_OVER_S - 1;
  alarmOff(0);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);   // not yet

  s_uptimeSec += 1;
  notifyAlarmState(0, false, false, false, 20.5f, 22.0f);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_ENDED, s_queue[0].event.type);
  TEST_ASSERT_EQUAL_FLOAT(20.5f, s_queue[0].event.a);   // the reading when it is sent

  // Said once
  s_uptimeSec += 3600;
  alarmOff(0);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
}

// The desk controller's fermenter sat at its limit for an afternoon and its
// alarm went on and off every few minutes. That must be ONE notification,
// not dozens.
void test_an_alarm_flapping_at_its_limit_sends_one_notification(void) {
  for (int cycle = 0; cycle < 20; cycle++) {
    alarmOn(0);
    s_uptimeSec += 180;                        // on for 3 minutes
    alarmOn(0);
    alarmOff(0);
    s_uptimeSec += NOTIFY_PROBLEM_OVER_S - 1;  // off for just under 10
    alarmOff(0);
  }
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_OVER, s_queue[0].event.type);
}

void test_a_new_alarm_after_one_has_ended_is_reported_again(void) {
  alarmOn(0);
  alarmOff(0);
  s_uptimeSec += NOTIFY_PROBLEM_OVER_S;
  alarmOff(0);
  alarmOn(0);
  TEST_ASSERT_EQUAL_UINT8(3, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_OVER,  s_queue[0].event.type);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_ENDED, s_queue[1].event.type);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_OVER,  s_queue[2].event.type);
}

void test_each_fermenter_alarm_is_tracked_on_its_own(void) {
  alarmOn(0);
  alarmOn(2);
  TEST_ASSERT_EQUAL_UINT8(2, s_queueCount);
  alarmOff(0);
  s_uptimeSec += NOTIFY_PROBLEM_OVER_S;
  alarmOff(0);
  alarmOn(2);   // still on: nothing new for fermenter 2
  TEST_ASSERT_EQUAL_UINT8(3, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_ENDED, s_queue[2].event.type);
  TEST_ASSERT_EQUAL_UINT8(0, s_queue[2].event.index);
}

void test_alarm_ended_carries_no_reading_when_the_fermenter_was_switched_off(void) {
  alarmOn(0);
  notifyAlarmState(0, false, false, false, TEMP_NONE, 0.0f);
  s_uptimeSec += NOTIFY_PROBLEM_OVER_S;
  notifyAlarmState(0, false, false, false, TEMP_NONE, 0.0f);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_ENDED, s_queue[1].event.type);
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, s_queue[1].event.a);
}

void test_switching_alarm_notifications_on_during_an_alarm_reports_it(void) {
  g_notifyConfig.events = NOTIFY_EVT_ALL & ~NOTIFY_EVT_ALARMS;
  alarmOn(0);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);

  g_notifyConfig.events = NOTIFY_EVT_ALL;
  alarmOn(0);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
}

void test_no_alarm_ended_for_an_alarm_whose_start_was_never_sent(void) {
  g_notifyConfig.enabled = false;
  alarmOn(0);
  g_notifyConfig.enabled = true;
  alarmOff(0);
  s_uptimeSec += NOTIFY_PROBLEM_OVER_S;
  alarmOff(0);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
}

void test_an_alarm_for_a_fermenter_that_does_not_exist_is_ignored(void) {
  notifyAlarmState(MAX_FERMENTERS, true, true, true, 30.0f, 22.0f);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
}

// ============================================================
// SENSOR PROBLEMS
// ============================================================

void test_a_lost_sensor_is_reported_once_with_the_right_event(void) {
  notifySensorState(NOTIFY_SENSOR_PROBE, 2, true);
  notifySensorState(NOTIFY_SENSOR_ISPINDEL, 1, true, 46.0f);
  notifySensorState(NOTIFY_SENSOR_TILT, 3, true, 25.0f);
  TEST_ASSERT_EQUAL_UINT8(3, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_PROBE_LOST, s_queue[0].event.type);
  TEST_ASSERT_EQUAL_UINT8(2, s_queue[0].event.index);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ISPINDEL_LOST, s_queue[1].event.type);
  TEST_ASSERT_EQUAL_UINT8(1, s_queue[1].event.index);
  TEST_ASSERT_EQUAL_FLOAT(46.0f, s_queue[1].event.a);
  TEST_ASSERT_EQUAL_INT(NOTIFY_TILT_LOST, s_queue[2].event.type);
  TEST_ASSERT_EQUAL_UINT8(3, s_queue[2].event.index);
  TEST_ASSERT_EQUAL_FLOAT(25.0f, s_queue[2].event.a);

  // The checks carry on every few seconds: nothing is repeated
  for (int n = 0; n < 50; n++) {
    s_uptimeSec += 5;
    notifySensorState(NOTIFY_SENSOR_PROBE, 2, true);
    notifySensorState(NOTIFY_SENSOR_ISPINDEL, 1, true, 47.0f);
    notifySensorState(NOTIFY_SENSOR_TILT, 3, true, 26.0f);
  }
  TEST_ASSERT_EQUAL_UINT8(3, s_queueCount);
}

void test_a_sensor_that_was_never_lost_reports_nothing(void) {
  for (int n = 0; n < 200; n++) {
    s_uptimeSec += 5;
    notifySensorState(NOTIFY_SENSOR_PROBE, 0, false);
  }
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
}

void test_a_sensor_is_reported_back_after_ten_steady_minutes(void) {
  notifySensorState(NOTIFY_SENSOR_ISPINDEL, 1, true, 46.0f);
  notifySensorState(NOTIFY_SENSOR_ISPINDEL, 1, false);
  s_uptimeSec += NOTIFY_PROBLEM_OVER_S - 1;
  notifySensorState(NOTIFY_SENSOR_ISPINDEL, 1, false);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);   // not yet

  s_uptimeSec += 1;
  notifySensorState(NOTIFY_SENSOR_ISPINDEL, 1, false);
  TEST_ASSERT_EQUAL_UINT8(2, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ISPINDEL_BACK, s_queue[1].event.type);
  TEST_ASSERT_EQUAL_UINT8(1, s_queue[1].event.index);
}

void test_a_probe_with_a_loose_wire_sends_one_notification(void) {
  for (int cycle = 0; cycle < 30; cycle++) {
    notifySensorState(NOTIFY_SENSOR_PROBE, 2, true);
    s_uptimeSec += 60;
    notifySensorState(NOTIFY_SENSOR_PROBE, 2, false);
    s_uptimeSec += 120;
    notifySensorState(NOTIFY_SENSOR_PROBE, 2, false);
  }
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_PROBE_LOST, s_queue[0].event.type);
}

void test_the_same_index_of_different_sensor_kinds_is_tracked_apart(void) {
  // Probe 1, iSpindel 1, Tilt 1 and fermenter 1's alarm are four separate things
  notifySensorState(NOTIFY_SENSOR_PROBE, 1, true);
  notifySensorState(NOTIFY_SENSOR_ISPINDEL, 1, true, 46.0f);
  notifySensorState(NOTIFY_SENSOR_TILT, 1, true, 25.0f);
  alarmOn(1);
  TEST_ASSERT_EQUAL_UINT8(4, s_queueCount);
}

void test_the_last_sensor_of_each_kind_has_a_tracker(void) {
  notifySensorState(NOTIFY_SENSOR_PROBE, MAX_PROBES - 1, true);
  notifySensorState(NOTIFY_SENSOR_ISPINDEL, MAX_ISPINDELS - 1, true, 46.0f);
  notifySensorState(NOTIFY_SENSOR_TILT, MAX_TILTS - 1, true, 25.0f);
  TEST_ASSERT_EQUAL_UINT8(3, s_queueCount);
  // ...and one past the end is ignored
  notifySensorState(NOTIFY_SENSOR_PROBE, MAX_PROBES, true);
  notifySensorState(NOTIFY_SENSOR_ISPINDEL, MAX_ISPINDELS, true, 46.0f);
  notifySensorState(NOTIFY_SENSOR_TILT, MAX_TILTS, true, 25.0f);
  TEST_ASSERT_EQUAL_UINT8(3, s_queueCount);
}

void test_switching_sensor_notifications_on_reports_a_sensor_already_lost(void) {
  g_notifyConfig.events = NOTIFY_EVT_ALL & ~NOTIFY_EVT_SENSORS;
  notifySensorState(NOTIFY_SENSOR_PROBE, 2, true);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
  g_notifyConfig.events = NOTIFY_EVT_ALL;
  notifySensorState(NOTIFY_SENSOR_PROBE, 2, true);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
}

// ============================================================
// RESTARTS
// ============================================================

void test_a_restart_after_a_crash_is_reported_with_its_reset_code(void) {
  notifyRestart(true, REASON_EXCEPTION_RST);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_RESTART, s_queue[0].event.type);
  TEST_ASSERT_EQUAL_UINT8(REASON_EXCEPTION_RST, s_queue[0].event.detail);

  // A watchdog reset, and a failed self-check (a software restart WITH a crash record)
  notifyRestart(true, REASON_WDT_RST);
  notifyRestart(true, REASON_SOFT_RESTART);
  TEST_ASSERT_EQUAL_UINT8(3, s_queueCount);
}

void test_the_power_coming_back_on_is_reported(void) {
  notifyRestart(false, REASON_DEFAULT_RST);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
  TEST_ASSERT_EQUAL_UINT8(REASON_DEFAULT_RST, s_queue[0].event.detail);
}

void test_a_restart_that_was_asked_for_is_not_reported(void) {
  notifyRestart(false, REASON_SOFT_RESTART);   // firmware update, or the Restart button
  notifyRestart(false, REASON_EXT_SYS_RST);    // the reset button
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
}

void test_a_restart_is_not_reported_when_its_box_is_not_ticked(void) {
  g_notifyConfig.events = NOTIFY_EVT_ALL & ~NOTIFY_EVT_RESTARTS;
  notifyRestart(true, REASON_EXCEPTION_RST);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
}

void test_a_restart_notification_waits_for_wifi(void) {
  // It is queued in setup(), often before WiFi has connected
  WiFi.connected = false;
  notifyRestart(true, REASON_EXCEPTION_RST);
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);

  WiFi.connected = true;
  s_uptimeSec += 20;
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_STRING("The controller restarted unexpectedly. Cause: crash.", g_httpTest.body);
}

// ============================================================
// ALERTS FOR THE WEBUI
// ============================================================

static NotifyAlert alert;

// Ask for one slot's alert. Returns false when the slot has nothing to show.
static bool getAlert(uint8_t slot) {
  memset(&alert, 0, sizeof(alert));
  return notifyGetAlert(slot, alert);
}

// How many slots have an alert to show
static int alertCount() {
  int count = 0;
  for (uint8_t slot = 0; slot < notifyAlertSlotCount(); slot++) {
    if (getAlert(slot)) count++;
  }
  return count;
}

void test_there_are_no_alerts_when_nothing_is_wrong(void) {
  TEST_ASSERT_EQUAL_INT(0, alertCount());
  // A slot past the end is empty too
  TEST_ASSERT_FALSE(getAlert(notifyAlertSlotCount()));
  TEST_ASSERT_FALSE(getAlert(255));
}

void test_an_alarm_gives_an_alert_with_the_notification_text(void) {
  alarmOn(1);
  TEST_ASSERT_EQUAL_INT(1, alertCount());
  TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS + 1));
  TEST_ASSERT_EQUAL_INT(ALERT_ALARM, alert.kind);
  TEST_ASSERT_EQUAL_STRING("Temperature alarm", alert.message->title);
  TEST_ASSERT_EQUAL_STRING("Lager fridge: 23.00 °C is above the 22.0 °C limit.", alert.message->body);
  TEST_ASSERT_EQUAL_STRING("0BADF00D-1-1000", alert.id);   // boot, slot, when it started
  TEST_ASSERT_EQUAL_UINT32(0, alert.ageSeconds);

  s_uptimeSec += 90;
  alarmOn(1);
  TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS + 1));
  TEST_ASSERT_EQUAL_UINT32(90, alert.ageSeconds);
}

void test_an_alarm_alert_shows_the_latest_reading(void) {
  notifyAlarmState(0, true, true, false, 23.0f, 22.0f);
  notifyAlarmState(0, true, true, true, 25.5f, 22.0f);   // now severe
  TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS));
  TEST_ASSERT_EQUAL_STRING("Severe temperature alarm", alert.message->title);
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 25.50 °C is above the 22.0 °C limit.", alert.message->body);

  notifyAlarmState(0, true, false, false, 9.0f, 10.0f);  // and now under its floor
  TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS));
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 9.00 °C is below the 10.0 °C limit.", alert.message->body);
}

// An alarm hovering at its limit flips on and off every few minutes. Its
// alert must keep one id the whole time, or a banner the brewer closed would
// come back at every flip.
void test_a_flapping_alarm_keeps_one_alert_id(void) {
  alarmOn(0);
  TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS));
  char firstId[sizeof(alert.id)];
  strlcpy(firstId, alert.id, sizeof(firstId));

  for (int flip = 0; flip < 5; flip++) {
    s_uptimeSec += 120; alarmOff(0);
    TEST_ASSERT_FALSE(getAlert(TRACK_ALARMS));   // no banner while it is off...
    s_uptimeSec += 120; alarmOn(0);
    TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS));    // ...and the same one when it is back
    TEST_ASSERT_EQUAL_STRING(firstId, alert.id);
  }
}

void test_an_alarm_after_ten_quiet_minutes_is_a_new_alert(void) {
  alarmOn(0);
  TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS));
  char firstId[sizeof(alert.id)];
  strlcpy(firstId, alert.id, sizeof(firstId));

  alarmOff(0);
  s_uptimeSec += NOTIFY_PROBLEM_OVER_S;
  alarmOff(0);   // the episode ends here
  s_uptimeSec += 5;
  alarmOn(0);
  TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS));
  TEST_ASSERT_FALSE(strcmp(firstId, alert.id) == 0);
}

// The banners are for everyone using the WebUI, not only those who set up
// notifications
void test_alerts_are_kept_with_notifications_off(void) {
  g_notifyConfig.enabled = false;
  alarmOn(0);
  notifySensorState(NOTIFY_SENSOR_PROBE, 2, true);
  notifyRestart(true, REASON_EXCEPTION_RST);
  notifyProfileFinished(1);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);   // nothing to send...
  TEST_ASSERT_EQUAL_INT(4, alertCount());     // ...but four banners
}

// Switching notifications, or any of their tick boxes, off and on during
// an alarm or a lost sensor must not touch the banners: same alerts, same ids
void test_changing_the_notification_settings_leaves_alerts_alone(void) {
  alarmOn(0);
  notifySensorState(NOTIFY_SENSOR_TILT, 0, true, 14.0f);
  notifyRestart(true, REASON_EXCEPTION_RST);
  notifyProfileFinished(1);
  TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS));
  char alarmId[sizeof(alert.id)];
  strlcpy(alarmId, alert.id, sizeof(alarmId));
  TEST_ASSERT_TRUE(getAlert(TRACK_TILTS));
  char tiltId[sizeof(alert.id)];
  strlcpy(tiltId, alert.id, sizeof(tiltId));

  const uint8_t settings[][2] = {   // { enabled, events }
    { false, NOTIFY_EVT_ALL }, { true, 0 }, { true, NOTIFY_EVT_ALL & ~NOTIFY_EVT_ALARMS },
    { true, NOTIFY_EVT_ALL & ~NOTIFY_EVT_SENSORS }, { false, 0 }, { true, NOTIFY_EVT_ALL },
  };
  for (const auto& setting : settings) {
    g_notifyConfig.enabled = setting[0];
    g_notifyConfig.events  = setting[1];
    s_uptimeSec += 30;
    alarmOn(0);   // the checks carry on as normal
    notifySensorState(NOTIFY_SENSOR_TILT, 0, true, 15.0f);

    TEST_ASSERT_EQUAL_INT(4, alertCount());
    TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS));
    TEST_ASSERT_EQUAL_STRING(alarmId, alert.id);
    TEST_ASSERT_TRUE(getAlert(TRACK_TILTS));
    TEST_ASSERT_EQUAL_STRING(tiltId, alert.id);
  }
}

// Notifications switched on part-way through an alarm still report it once,
// and still send "alarm ended" when it is over
void test_tracking_alerts_does_not_change_what_is_sent(void) {
  g_notifyConfig.enabled = false;
  alarmOn(0);
  g_notifyConfig.enabled = true;
  alarmOn(0);
  alarmOn(0);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_OVER, s_queue[0].event.type);

  alarmOff(0);
  s_uptimeSec += NOTIFY_PROBLEM_OVER_S;
  alarmOff(0);
  TEST_ASSERT_EQUAL_UINT8(2, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_ENDED, s_queue[1].event.type);
}

void test_a_lost_sensor_gives_an_alert_with_the_latest_minutes(void) {
  notifySensorState(NOTIFY_SENSOR_TILT, 0, true, 14.0f);
  TEST_ASSERT_TRUE(getAlert(TRACK_TILTS));
  TEST_ASSERT_EQUAL_INT(ALERT_SENSOR, alert.kind);
  TEST_ASSERT_EQUAL_STRING("Tilt stopped reporting", alert.message->title);
  TEST_ASSERT_EQUAL_STRING("Red Tilt: no reading for 14 min. Its readings are no longer used.", alert.message->body);

  notifySensorState(NOTIFY_SENSOR_TILT, 0, true, 15.0f);
  TEST_ASSERT_TRUE(getAlert(TRACK_TILTS));
  TEST_ASSERT_EQUAL_STRING("Red Tilt: no reading for 15 min. Its readings are no longer used.", alert.message->body);

  notifySensorState(NOTIFY_SENSOR_ISPINDEL, 1, true, 70.0f);
  TEST_ASSERT_TRUE(getAlert(TRACK_ISPINDELS + 1));
  TEST_ASSERT_EQUAL_STRING("ispindel-4: no report for 70 min. Its readings are no longer used.", alert.message->body);

  notifySensorState(NOTIFY_SENSOR_PROBE, 2, true);
  TEST_ASSERT_TRUE(getAlert(TRACK_PROBES + 2));
  TEST_ASSERT_EQUAL_STRING("Beer Temp is not responding. Its readings are no longer used.", alert.message->body);

  // Reporting again: the banner goes at once
  notifySensorState(NOTIFY_SENSOR_TILT, 0, false);
  TEST_ASSERT_FALSE(getAlert(TRACK_TILTS));
}

// The sensor code stops checking a sensor once it is unassigned, so the
// alert must not wait for a "reporting again" that will never come
void test_a_lost_sensor_that_is_unassigned_has_no_alert(void) {
  notifySensorState(NOTIFY_SENSOR_PROBE, 2, true);
  notifySensorState(NOTIFY_SENSOR_ISPINDEL, 1, true, 70.0f);
  notifySensorState(NOTIFY_SENSOR_TILT, 3, true, 20.0f);
  TEST_ASSERT_EQUAL_INT(3, alertCount());

  g_probes[2].fermenter    = PROBE_UNASSIGNED;
  g_iSpindels[1].fermenter = PROBE_UNASSIGNED;
  g_tilts[3].fermenter     = PROBE_UNASSIGNED;
  TEST_ASSERT_EQUAL_INT(0, alertCount());
}

void test_an_unexpected_restart_gives_an_alert_for_the_whole_boot(void) {
  notifyRestart(true, REASON_WDT_RST);
  TEST_ASSERT_TRUE(getAlert(SLOT_RESTART));
  TEST_ASSERT_EQUAL_INT(ALERT_RESTART, alert.kind);
  TEST_ASSERT_EQUAL_STRING("Controller restarted", alert.message->title);
  TEST_ASSERT_EQUAL_STRING("The controller restarted unexpectedly. Cause: hardware watchdog.", alert.message->body);
  TEST_ASSERT_EQUAL_UINT32(1000, alert.ageSeconds);   // seconds since boot
  TEST_ASSERT_EQUAL_STRING("0BADF00D-28-0", alert.id);

  s_uptimeSec += 3 * 86400;
  TEST_ASSERT_TRUE(getAlert(SLOT_RESTART));
}

void test_a_restart_that_was_asked_for_gives_no_alert(void) {
  notifyRestart(false, REASON_SOFT_RESTART);
  notifyRestart(false, REASON_EXT_SYS_RST);
  TEST_ASSERT_EQUAL_INT(0, alertCount());
}

// Every alert but a release belongs to one boot: after a restart, a problem
// that is still there is a new one, so a closed banner for it shows again
void test_alert_ids_change_after_a_restart(void) {
  alarmOn(0);
  TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS));
  TEST_ASSERT_EQUAL_STRING("0BADF00D-0-1000", alert.id);
  s_bootId = 0x12345678;
  TEST_ASSERT_TRUE(getAlert(TRACK_ALARMS));
  TEST_ASSERT_EQUAL_STRING("12345678-0-1000", alert.id);
}

void test_a_finished_profile_gives_an_alert_until_a_new_one_starts(void) {
  notifyProfileFinished(1);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);   // the notification still goes
  TEST_ASSERT_EQUAL_INT(NOTIFY_PROFILE_FINISHED, s_queue[0].event.type);
  TEST_ASSERT_EQUAL_UINT8(1, s_queue[0].event.index);

  s_uptimeSec += 600;
  TEST_ASSERT_TRUE(getAlert(SLOT_PROFILES + 1));
  TEST_ASSERT_EQUAL_INT(ALERT_PROFILE, alert.kind);
  TEST_ASSERT_EQUAL_STRING("Profile finished", alert.message->title);
  TEST_ASSERT_EQUAL_STRING("Lager fridge: the profile has finished.", alert.message->body);
  TEST_ASSERT_EQUAL_UINT32(600, alert.ageSeconds);

  notifyProfileStarted(0);   // another fermenter's start changes nothing
  TEST_ASSERT_TRUE(getAlert(SLOT_PROFILES + 1));
  notifyProfileStarted(1);
  TEST_ASSERT_FALSE(getAlert(SLOT_PROFILES + 1));

  // A fermenter that doesn't exist is ignored
  notifyProfileFinished(MAX_FERMENTERS);
  notifyProfileStarted(MAX_FERMENTERS);
  TEST_ASSERT_EQUAL_INT(0, alertCount());
}

// A release's id has no boot in it, so a banner closed for it stays closed
// after a restart, until there is a newer release
void test_a_new_release_gives_an_alert_named_after_the_version(void) {
  strlcpy(g_updateStatus.latestVersion, "0.11.0", sizeof(g_updateStatus.latestVersion));
  TEST_ASSERT_FALSE(getAlert(SLOT_UPDATE));   // not newer than this firmware
  g_updateStatus.updateAvailable = true;
  TEST_ASSERT_TRUE(getAlert(SLOT_UPDATE));
  TEST_ASSERT_EQUAL_INT(ALERT_UPDATE, alert.kind);
  TEST_ASSERT_EQUAL_STRING("Firmware update available", alert.message->title);
  TEST_ASSERT_EQUAL_STRING("update-0.11.0", alert.id);
  TEST_ASSERT_EQUAL_UINT32(0, alert.ageSeconds);
  s_bootId = 0x12345678;
  TEST_ASSERT_TRUE(getAlert(SLOT_UPDATE));
  TEST_ASSERT_EQUAL_STRING("update-0.11.0", alert.id);
}

// The longest ids: the largest boot id, slot and time, and the longest
// version. snprintf_P cuts an id that doesn't fit, and two cut ids could
// then look the same, so check they arrive whole.
void test_the_longest_alert_ids_fit(void) {
  s_bootId    = 0xFFFFFFFF;
  s_uptimeSec = 0xFFFFFFFF;
  notifySensorState(NOTIFY_SENSOR_TILT, MAX_TILTS - 1, true, 20.0f);
  TEST_ASSERT_TRUE(getAlert(TRACK_TILTS + MAX_TILTS - 1));
  TEST_ASSERT_EQUAL_STRING("FFFFFFFF-23-4294967295", alert.id);

  memset(g_updateStatus.latestVersion, '9', sizeof(g_updateStatus.latestVersion) - 1);
  g_updateStatus.updateAvailable = true;
  TEST_ASSERT_TRUE(getAlert(SLOT_UPDATE));
  TEST_ASSERT_EQUAL_UINT(strlen("update-") + sizeof(g_updateStatus.latestVersion) - 1, strlen(alert.id));
  TEST_ASSERT_TRUE(strlen(alert.id) < sizeof(alert.id) - 1);
}

void test_every_kind_of_alert_at_once(void) {
  for (int i = 0; i < MAX_FERMENTERS; i++) { alarmOn(i); notifyProfileFinished(i); }
  for (int i = 0; i < MAX_PROBES; i++)     notifySensorState(NOTIFY_SENSOR_PROBE, i, true);
  for (int i = 0; i < MAX_ISPINDELS; i++)  notifySensorState(NOTIFY_SENSOR_ISPINDEL, i, true, 30.0f);
  for (int i = 0; i < MAX_TILTS; i++)      notifySensorState(NOTIFY_SENSOR_TILT, i, true, 30.0f);
  notifyRestart(false, REASON_DEFAULT_RST);
  strlcpy(g_updateStatus.latestVersion, "0.11.0", sizeof(g_updateStatus.latestVersion));
  g_updateStatus.updateAvailable = true;
  TEST_ASSERT_EQUAL_INT(notifyAlertSlotCount(), alertCount());
}

// ============================================================
// SENDING
// ============================================================

void test_nothing_waiting_sends_nothing(void) {
  TEST_ASSERT_FALSE(notifySendDue());
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_UINT8(0xFF, s_lastCheckpoint);
}

void test_a_notification_is_posted_as_ntfy_expects(void) {
  notifyEvent(NOTIFY_ALARM_OVER, 0, 1, 32.0f, 29.0f);
  TEST_ASSERT_TRUE(notifySendDue());
  notifyLoop();

  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_STRING("http://ntfy.sh/my-topic", g_httpTest.url);
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 32.00 °C is above the 29.0 °C limit.", g_httpTest.body);
  TEST_ASSERT_EQUAL_STRING(
    "Content-Type: text/plain; charset=utf-8\n"
    "Title: Severe temperature alarm\n"
    "Priority: 5\n"
    "Tags: warning\n",
    g_httpTest.headers);
  TEST_ASSERT_EQUAL_UINT8(CP_NOTIFY, s_lastCheckpoint);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
  TEST_ASSERT_FALSE(notifySendDue());
  TEST_ASSERT_EQUAL_STRING("[NOTIFY] Sent: Severe temperature alarm (HTTP 200)", s_lastLog);
}

void test_one_notification_is_sent_per_pass_oldest_first(void) {
  notifyEvent(NOTIFY_PROBE_LOST, 2);
  notifyEvent(NOTIFY_PROFILE_FINISHED, 1);

  notifyLoop();
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_STRING("Beer Temp is not responding. Its readings are no longer used.", g_httpTest.body);

  notifyLoop();
  TEST_ASSERT_EQUAL_INT(2, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_STRING("Lager fridge: the profile has finished.", g_httpTest.body);

  notifyLoop();
  TEST_ASSERT_EQUAL_INT(2, g_httpTest.postCount);
}

void test_the_log_never_holds_the_url(void) {
  // With ntfy the topic name in the URL is what keeps the messages private
  g_httpTest.nextStatus = 403;
  notifyEvent(NOTIFY_PROFILE_FINISHED, 0);
  notifyLoop();
  TEST_ASSERT_NULL(strstr(s_lastLog, "my-topic"));
  g_httpTest.nextStatus = 200;
  s_uptimeSec += NOTIFY_RETRY_S;
  notifyLoop();
  TEST_ASSERT_NULL(strstr(s_lastLog, "my-topic"));
}

void test_a_failed_send_is_tried_again_a_minute_later(void) {
  g_httpTest.nextStatus = 503;
  notifyEvent(NOTIFY_PROFILE_FINISHED, 0);
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);   // still waiting
  TEST_ASSERT_EQUAL_STRING("[NOTIFY] Profile finished: HTTP 503 - trying again in a minute", s_lastLog);

  // Not before the minute is up
  s_uptimeSec += NOTIFY_RETRY_S - 1;
  TEST_ASSERT_FALSE(notifySendDue());
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);

  s_uptimeSec += 1;
  g_httpTest.nextStatus = 200;
  TEST_ASSERT_TRUE(notifySendDue());
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(2, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
}

void test_a_notification_is_given_up_on_after_three_tries(void) {
  g_httpTest.nextStatus = -1;   // no connection
  notifyEvent(NOTIFY_PROFILE_FINISHED, 0);
  notifyEvent(NOTIFY_PROBE_BACK, 2);

  notifyLoop();
  s_uptimeSec += NOTIFY_RETRY_S;
  notifyLoop();
  TEST_ASSERT_EQUAL_UINT8(2, s_queueCount);
  s_uptimeSec += NOTIFY_RETRY_S;
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(3, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);   // the first one is gone
  TEST_ASSERT_EQUAL_STRING("[NOTIFY] Profile finished: no reply (error -1) - given up", s_lastLog);

  // The next one starts with a clean count and is sent straight away
  g_httpTest.nextStatus = 200;
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(4, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_STRING("Beer Temp is responding again. Its readings are used.", g_httpTest.body);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
}

void test_a_redirect_counts_as_a_failure(void) {
  // e.g. a server that only answers on https
  g_httpTest.nextStatus = 301;
  notifyEvent(NOTIFY_PROFILE_FINISHED, 0);
  notifyLoop();
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
}

void test_nothing_is_sent_while_wifi_is_down(void) {
  WiFi.connected = false;
  notifyEvent(NOTIFY_PROFILE_FINISHED, 0);
  TEST_ASSERT_FALSE(notifySendDue());
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);   // kept for when WiFi is back

  WiFi.connected = true;
  s_uptimeSec += 600;
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
}

void test_an_event_older_than_an_hour_is_dropped_unsent(void) {
  WiFi.connected = false;
  notifyEvent(NOTIFY_PROFILE_FINISHED, 0);
  s_uptimeSec += NOTIFY_MAX_AGE_S;
  notifyLoop();
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);   // exactly an hour: still kept

  s_uptimeSec += 1;
  WiFi.connected = true;
  notifyLoop();
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);
}

void test_switching_notifications_off_clears_what_was_waiting(void) {
  notifyEvent(NOTIFY_PROFILE_FINISHED, 0);
  notifyEvent(NOTIFY_PROBE_BACK, 2);
  g_notifyConfig.enabled = false;
  TEST_ASSERT_FALSE(notifySendDue());
  notifyLoop();
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);
}

void test_an_event_that_cannot_be_built_does_not_block_the_queue(void) {
  notifyEvent(NOTIFY_PROFILE_FINISHED, MAX_FERMENTERS);   // no such fermenter
  notifyEvent(NOTIFY_PROBE_BACK, 2);
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_STRING("Beer Temp is responding again. Its readings are used.", g_httpTest.body);
}

// ============================================================
// THE TEST NOTIFICATION
// ============================================================

void test_the_test_notification_is_sent_at_once_and_returns_the_status(void) {
  g_notifyConfig.enabled = false;   // the test works before notifications are switched on
  g_notifyConfig.events  = 0;
  TEST_ASSERT_EQUAL_INT(200, notifySendTest());
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_STRING("http://ntfy.sh/my-topic", g_httpTest.url);
  TEST_ASSERT_EQUAL_STRING("Test notification from your OurBrewbot controller.", g_httpTest.body);
  TEST_ASSERT_NOT_NULL(strstr(g_httpTest.headers, "Title: OurBrewbot test\n"));
}

void test_a_failed_test_notification_returns_the_error_and_is_not_retried(void) {
  g_httpTest.nextStatus = 404;
  TEST_ASSERT_EQUAL_INT(404, notifySendTest());
  TEST_ASSERT_EQUAL_STRING("[NOTIFY] OurBrewbot test: HTTP 404 - test not delivered", s_lastLog);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
  notifyLoop();
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_over_temperature_alarm_message);
  RUN_TEST(test_under_temperature_alarm_message);
  RUN_TEST(test_severe_alarm_is_urgent_and_says_so);
  RUN_TEST(test_a_reading_just_past_its_limit_does_not_look_equal_to_it);
  RUN_TEST(test_alarm_message_uses_fahrenheit_when_the_controller_does);
  RUN_TEST(test_alarm_ended_message_gives_the_temperature);
  RUN_TEST(test_alarm_ended_without_a_reading_says_so);
  RUN_TEST(test_probe_messages);
  RUN_TEST(test_ispindel_messages);
  RUN_TEST(test_tilt_messages);
  RUN_TEST(test_restart_message_names_the_cause);
  RUN_TEST(test_profile_messages);
  RUN_TEST(test_update_message_names_both_versions);
  RUN_TEST(test_test_message);
  RUN_TEST(test_an_empty_name_is_replaced_by_a_number);
  RUN_TEST(test_an_event_for_something_that_does_not_exist_builds_nothing);
  RUN_TEST(test_worst_case_messages_all_fit_the_buffer);
  RUN_TEST(test_headings_fit_their_fields);
  RUN_TEST(test_extreme_numbers_are_kept_to_a_fixed_width);

  RUN_TEST(test_nothing_is_queued_when_notifications_are_off);
  RUN_TEST(test_nothing_is_queued_for_a_kind_of_event_that_is_not_ticked);
  RUN_TEST(test_each_kind_of_event_follows_its_own_tick_box);
  RUN_TEST(test_the_test_message_is_never_queued);
  RUN_TEST(test_a_full_queue_drops_the_new_event_and_logs_it);

  RUN_TEST(test_an_alarm_is_reported_once_when_it_starts);
  RUN_TEST(test_an_under_temperature_alarm_is_reported_as_one);
  RUN_TEST(test_a_mild_alarm_that_turns_severe_is_reported_again);
  RUN_TEST(test_alarm_ended_waits_until_the_alarm_has_been_off_ten_minutes);
  RUN_TEST(test_an_alarm_flapping_at_its_limit_sends_one_notification);
  RUN_TEST(test_a_new_alarm_after_one_has_ended_is_reported_again);
  RUN_TEST(test_each_fermenter_alarm_is_tracked_on_its_own);
  RUN_TEST(test_alarm_ended_carries_no_reading_when_the_fermenter_was_switched_off);
  RUN_TEST(test_switching_alarm_notifications_on_during_an_alarm_reports_it);
  RUN_TEST(test_no_alarm_ended_for_an_alarm_whose_start_was_never_sent);
  RUN_TEST(test_an_alarm_for_a_fermenter_that_does_not_exist_is_ignored);

  RUN_TEST(test_a_lost_sensor_is_reported_once_with_the_right_event);
  RUN_TEST(test_a_sensor_that_was_never_lost_reports_nothing);
  RUN_TEST(test_a_sensor_is_reported_back_after_ten_steady_minutes);
  RUN_TEST(test_a_probe_with_a_loose_wire_sends_one_notification);
  RUN_TEST(test_the_same_index_of_different_sensor_kinds_is_tracked_apart);
  RUN_TEST(test_the_last_sensor_of_each_kind_has_a_tracker);
  RUN_TEST(test_switching_sensor_notifications_on_reports_a_sensor_already_lost);

  RUN_TEST(test_a_restart_after_a_crash_is_reported_with_its_reset_code);
  RUN_TEST(test_the_power_coming_back_on_is_reported);
  RUN_TEST(test_a_restart_that_was_asked_for_is_not_reported);
  RUN_TEST(test_a_restart_is_not_reported_when_its_box_is_not_ticked);
  RUN_TEST(test_a_restart_notification_waits_for_wifi);

  RUN_TEST(test_there_are_no_alerts_when_nothing_is_wrong);
  RUN_TEST(test_an_alarm_gives_an_alert_with_the_notification_text);
  RUN_TEST(test_an_alarm_alert_shows_the_latest_reading);
  RUN_TEST(test_a_flapping_alarm_keeps_one_alert_id);
  RUN_TEST(test_an_alarm_after_ten_quiet_minutes_is_a_new_alert);
  RUN_TEST(test_alerts_are_kept_with_notifications_off);
  RUN_TEST(test_changing_the_notification_settings_leaves_alerts_alone);
  RUN_TEST(test_tracking_alerts_does_not_change_what_is_sent);
  RUN_TEST(test_a_lost_sensor_gives_an_alert_with_the_latest_minutes);
  RUN_TEST(test_a_lost_sensor_that_is_unassigned_has_no_alert);
  RUN_TEST(test_an_unexpected_restart_gives_an_alert_for_the_whole_boot);
  RUN_TEST(test_a_restart_that_was_asked_for_gives_no_alert);
  RUN_TEST(test_alert_ids_change_after_a_restart);
  RUN_TEST(test_a_finished_profile_gives_an_alert_until_a_new_one_starts);
  RUN_TEST(test_a_new_release_gives_an_alert_named_after_the_version);
  RUN_TEST(test_the_longest_alert_ids_fit);
  RUN_TEST(test_every_kind_of_alert_at_once);

  RUN_TEST(test_nothing_waiting_sends_nothing);
  RUN_TEST(test_a_notification_is_posted_as_ntfy_expects);
  RUN_TEST(test_one_notification_is_sent_per_pass_oldest_first);
  RUN_TEST(test_the_log_never_holds_the_url);
  RUN_TEST(test_a_failed_send_is_tried_again_a_minute_later);
  RUN_TEST(test_a_notification_is_given_up_on_after_three_tries);
  RUN_TEST(test_a_redirect_counts_as_a_failure);
  RUN_TEST(test_nothing_is_sent_while_wifi_is_down);
  RUN_TEST(test_an_event_older_than_an_hour_is_dropped_unsent);
  RUN_TEST(test_switching_notifications_off_clears_what_was_waiting);
  RUN_TEST(test_an_event_that_cannot_be_built_does_not_block_the_queue);

  RUN_TEST(test_the_test_notification_is_sent_at_once_and_returns_the_status);
  RUN_TEST(test_a_failed_test_notification_returns_the_error_and_is_not_retried);

  return UNITY_END();
}
