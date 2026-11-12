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
  memset(s_alarmSent, 0, sizeof(s_alarmSent));
  memset(s_alarmOffSince, 0, sizeof(s_alarmOffSince));

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
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 29.24 C is above the 29.0 C limit.", msg.body);
  TEST_ASSERT_EQUAL_STRING("warning", msg.tags);
  TEST_ASSERT_EQUAL_UINT8(4, msg.priority);
}

void test_under_temperature_alarm_message(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_UNDER, 1, 0, 8.5f, 10.0f));
  TEST_ASSERT_EQUAL_STRING("Temperature alarm", msg.title);
  TEST_ASSERT_EQUAL_STRING("Lager fridge: 8.50 C is below the 10.0 C limit.", msg.body);
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
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 29.05 C is above the 29.0 C limit.", msg.body);
}

void test_alarm_message_uses_fahrenheit_when_the_controller_does(void) {
  g_globalConfig.unit = 2;   // Fahrenheit
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_OVER, 0, 0, 30.0f, 25.0f));
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 86.00 F is above the 77.0 F limit.", msg.body);
}

void test_alarm_ended_message_gives_the_temperature(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_ENDED, 0, 0, 20.1f));
  TEST_ASSERT_EQUAL_STRING("Temperature alarm ended", msg.title);
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: back within its limits at 20.10 C.", msg.body);
  TEST_ASSERT_EQUAL_STRING("white_check_mark", msg.tags);
  TEST_ASSERT_EQUAL_UINT8(3, msg.priority);
}

void test_alarm_ended_without_a_reading_says_so(void) {
  // The alarm also ends when the fermenter is switched off or its probe is lost
  TEST_ASSERT_TRUE(build(NOTIFY_ALARM_ENDED, 0, 0, TEMP_NONE));
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: the alarm has ended (no temperature reading).", msg.body);
}

void test_probe_messages(void) {
  TEST_ASSERT_TRUE(build(NOTIFY_PROBE_LOST, 2, 0, 5.0f));
  TEST_ASSERT_EQUAL_STRING("Temperature probe not responding", msg.title);
  TEST_ASSERT_EQUAL_STRING("Beer Temp has not responded for 5 min.", msg.body);
  TEST_ASSERT_EQUAL_UINT8(4, msg.priority);

  TEST_ASSERT_TRUE(build(NOTIFY_PROBE_BACK, 2));
  TEST_ASSERT_EQUAL_STRING("Temperature probe responding again", msg.title);
  TEST_ASSERT_EQUAL_STRING("Beer Temp is reading again.", msg.body);
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
  TEST_ASSERT_EQUAL_STRING("Probe 3 is reading again.", msg.body);

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
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 999.90 C is above the -999.9 C limit.", msg.body);
  TEST_ASSERT_TRUE(build(NOTIFY_PROBE_LOST, 2, 0, 3.0e38f));
  TEST_ASSERT_EQUAL_STRING("Beer Temp has not responded for 99999 min.", msg.body);
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
  s_uptimeSec += NOTIFY_ALARM_ENDED_S - 1;
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
    s_uptimeSec += NOTIFY_ALARM_ENDED_S - 1;   // off for just under 10
    alarmOff(0);
  }
  TEST_ASSERT_EQUAL_UINT8(1, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_OVER, s_queue[0].event.type);
}

void test_a_new_alarm_after_one_has_ended_is_reported_again(void) {
  alarmOn(0);
  alarmOff(0);
  s_uptimeSec += NOTIFY_ALARM_ENDED_S;
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
  s_uptimeSec += NOTIFY_ALARM_ENDED_S;
  alarmOff(0);
  alarmOn(2);   // still on: nothing new for fermenter 2
  TEST_ASSERT_EQUAL_UINT8(3, s_queueCount);
  TEST_ASSERT_EQUAL_INT(NOTIFY_ALARM_ENDED, s_queue[2].event.type);
  TEST_ASSERT_EQUAL_UINT8(0, s_queue[2].event.index);
}

void test_alarm_ended_carries_no_reading_when_the_fermenter_was_switched_off(void) {
  alarmOn(0);
  notifyAlarmState(0, false, false, false, TEMP_NONE, 0.0f);
  s_uptimeSec += NOTIFY_ALARM_ENDED_S;
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
  s_uptimeSec += NOTIFY_ALARM_ENDED_S;
  alarmOff(0);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
}

void test_an_alarm_for_a_fermenter_that_does_not_exist_is_ignored(void) {
  notifyAlarmState(MAX_FERMENTERS, true, true, true, 30.0f, 22.0f);
  TEST_ASSERT_EQUAL_UINT8(0, s_queueCount);
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
  TEST_ASSERT_EQUAL_STRING("Fermenter 1: 32.00 C is above the 29.0 C limit.", g_httpTest.body);
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
  notifyEvent(NOTIFY_PROBE_LOST, 2, 0, 5.0f);
  notifyEvent(NOTIFY_PROFILE_FINISHED, 1);

  notifyLoop();
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_STRING("Beer Temp has not responded for 5 min.", g_httpTest.body);

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
  TEST_ASSERT_EQUAL_STRING("Beer Temp is reading again.", g_httpTest.body);
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
  TEST_ASSERT_EQUAL_STRING("Beer Temp is reading again.", g_httpTest.body);
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
