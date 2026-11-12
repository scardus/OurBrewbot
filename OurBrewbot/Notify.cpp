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

/*
 * Notify.cpp — Push notifications over plain HTTP (see Notify.h)
 */

#include "Notify.h"
#include "Config.h"
#include "Crash.h"
#include "Temperatures.h"
#include "Tilt.h"
#include "UpdateCheck.h"
#include "Uptime.h"
#include "Version.h"
#include "Log.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <user_interface.h>   // REASON_* reset reason codes

// The core applies this to the DNS lookup, the connection and the reply
// separately, so one failed request can hold up the loop for about 15 s
// (the same as a brew service report).
#define NOTIFY_TIMEOUT_MS   5000

#define NOTIFY_QUEUE_SIZE   8      // events that can wait at once
#define NOTIFY_MAX_TRIES    3      // tries before a notification is given up on
#define NOTIFY_RETRY_S      60     // wait between tries
#define NOTIFY_MAX_AGE_S    3600   // an event still unsent after an hour is old news

// ntfy priorities
#define PRIORITY_LOW      2
#define PRIORITY_NORMAL   3
#define PRIORITY_HIGH     4
#define PRIORITY_URGENT   5

// ============================================================
// THE QUEUE
//
// A plain array, oldest event first. It lives in static memory (not on the
// heap) so queueing an event can never fail for lack of memory or fragment
// the heap.
// ============================================================

struct QueuedEvent {
  NotifyEventData event;
  uint32_t        queuedAt;   // uptimeStamp() when it was queued
};

static QueuedEvent s_queue[NOTIFY_QUEUE_SIZE];
static uint8_t     s_queueCount = 0;
static uint8_t     s_tries      = 0;   // failed tries for the event at the front
static uint32_t    s_lastTryAt  = 0;   // uptimeStamp() of the last failed try

// The message being sent. Static rather than on the stack: the loop stack is
// only 4 KB and the HTTP request below it needs most of what is free.
static NotifyMessage s_message;

// Take the oldest event off the queue and move the others up
static void removeFront() {
  for (uint8_t i = 1; i < s_queueCount; i++) {
    s_queue[i - 1] = s_queue[i];
  }
  s_queueCount--;
  s_tries = 0;
}

// ============================================================
// MESSAGE TEXT
// ============================================================

// Which settings tick box an event belongs to
static uint8_t eventGroup(NotifyEvent type) {
  switch (type) {
    case NOTIFY_ALARM_OVER:
    case NOTIFY_ALARM_UNDER:
    case NOTIFY_ALARM_ENDED:       return NOTIFY_EVT_ALARMS;
    case NOTIFY_PROBE_LOST:
    case NOTIFY_PROBE_BACK:
    case NOTIFY_ISPINDEL_LOST:
    case NOTIFY_ISPINDEL_BACK:
    case NOTIFY_TILT_LOST:
    case NOTIFY_TILT_BACK:         return NOTIFY_EVT_SENSORS;
    case NOTIFY_RESTART:           return NOTIFY_EVT_RESTARTS;
    case NOTIFY_STEP_COMPLETE:
    case NOTIFY_PROFILE_FINISHED:  return NOTIFY_EVT_PROFILE;
    case NOTIFY_UPDATE_AVAILABLE:  return NOTIFY_EVT_UPDATES;
    case NOTIFY_TEST:              return 0;   // not queued, so no tick box
  }
  return 0;
}

// All the fixed text below is kept in flash with PSTR(). On this chip an
// ordinary "string" is copied into RAM at boot and stays there, and the
// notification texts together would take over 1 KB of it.

// Fill in the title, tag and priority. Titles are fixed ASCII text because
// they travel as an HTTP header; names typed by the user only ever go in the
// body.
static void setHeading(NotifyMessage& msg, PGM_P title, PGM_P tags, uint8_t priority) {
  strncpy_P(msg.title, title, sizeof(msg.title) - 1);
  msg.title[sizeof(msg.title) - 1] = '\0';
  strncpy_P(msg.tags, tags, sizeof(msg.tags) - 1);
  msg.tags[sizeof(msg.tags) - 1] = '\0';
  msg.priority = priority;
}

// The heading for each kind of event
static void setHeadingFor(const NotifyEventData& event, NotifyMessage& msg) {
  bool severe = (event.detail != 0);
  switch (event.type) {
    case NOTIFY_ALARM_OVER:
    case NOTIFY_ALARM_UNDER:
      if (severe) setHeading(msg, PSTR("Severe temperature alarm"), PSTR("warning"), PRIORITY_URGENT);
      else        setHeading(msg, PSTR("Temperature alarm"),        PSTR("warning"), PRIORITY_HIGH);
      break;
    case NOTIFY_ALARM_ENDED:
      setHeading(msg, PSTR("Temperature alarm ended"), PSTR("white_check_mark"), PRIORITY_NORMAL);
      break;
    case NOTIFY_PROBE_LOST:
      setHeading(msg, PSTR("Temperature probe not responding"), PSTR("warning"), PRIORITY_HIGH);
      break;
    case NOTIFY_PROBE_BACK:
      setHeading(msg, PSTR("Temperature probe responding again"), PSTR("white_check_mark"), PRIORITY_NORMAL);
      break;
    case NOTIFY_ISPINDEL_LOST:
      setHeading(msg, PSTR("iSpindel stopped reporting"), PSTR("warning"), PRIORITY_NORMAL);
      break;
    case NOTIFY_ISPINDEL_BACK:
      setHeading(msg, PSTR("iSpindel reporting again"), PSTR("white_check_mark"), PRIORITY_NORMAL);
      break;
    case NOTIFY_TILT_LOST:
      setHeading(msg, PSTR("Tilt stopped reporting"), PSTR("warning"), PRIORITY_NORMAL);
      break;
    case NOTIFY_TILT_BACK:
      setHeading(msg, PSTR("Tilt reporting again"), PSTR("white_check_mark"), PRIORITY_NORMAL);
      break;
    case NOTIFY_RESTART:
      setHeading(msg, PSTR("Controller restarted"), PSTR("warning"), PRIORITY_HIGH);
      break;
    case NOTIFY_STEP_COMPLETE:
      setHeading(msg, PSTR("Profile step complete"), PSTR("beer"), PRIORITY_NORMAL);
      break;
    case NOTIFY_PROFILE_FINISHED:
      setHeading(msg, PSTR("Profile finished"), PSTR("beer"), PRIORITY_NORMAL);
      break;
    case NOTIFY_UPDATE_AVAILABLE:
      setHeading(msg, PSTR("Firmware update available"), PSTR("arrow_up"), PRIORITY_LOW);
      break;
    case NOTIFY_TEST:
      setHeading(msg, PSTR("OurBrewbot test"), PSTR("beer"), PRIORITY_NORMAL);
      break;
  }
}

// A temperature in the unit the brewer chose, kept to a width the message
// buffer was sized for (-999.9 to 999.9). Real readings are far inside that.
// Readings are printed to two decimal places and limits to one: with one
// place each, 29.05 against a limit of 29.0 read "29.0 C is above the 29.0 C
// limit".
static float shownTemp(float celsius) {
  float t = toDisplayTemp(celsius);
  if (t >  999.9f) t =  999.9f;
  if (t < -999.9f) t = -999.9f;
  return t;
}

static char unitLetter() {
  return (g_globalConfig.unit == UNIT_CELSIUS) ? 'C' : 'F';
}

// A count of minutes, kept to five digits for the same reason
static unsigned long shownMinutes(float minutes) {
  if (minutes < 0.0f)     return 0;
  if (minutes > 99999.0f) return 99999;
  return (unsigned long)minutes;
}

// A profile step number, kept to what a uint8_t step can be
static unsigned shownStep(float step) {
  if (step < 0.0f)   return 0;
  if (step > 255.0f) return 255;
  return (unsigned)step;
}

// The names below are the ones typed by the user, or text like "Fermenter 2"
// when the name was left empty. They share one buffer for the numbered text,
// which is fine because a message only ever names one thing.
static char s_numberedName[16];

static const char* fermenterName(uint8_t i) {
  if (g_fermenters[i].fermenterName[0] != '\0') return g_fermenters[i].fermenterName;
  snprintf_P(s_numberedName, sizeof(s_numberedName), PSTR("Fermenter %u"), (unsigned)(i + 1));
  return s_numberedName;
}

static const char* probeName(uint8_t i) {
  if (g_probes[i].probeName[0] != '\0') return g_probes[i].probeName;
  snprintf_P(s_numberedName, sizeof(s_numberedName), PSTR("Probe %u"), (unsigned)(i + 1));
  return s_numberedName;
}

static const char* iSpindelName(uint8_t i) {
  if (g_iSpindels[i].name[0] != '\0') return g_iSpindels[i].name;
  snprintf_P(s_numberedName, sizeof(s_numberedName), PSTR("iSpindel %u"), (unsigned)(i + 1));
  return s_numberedName;
}

// Why the controller restarted, in plain words
static PGM_P restartReasonText(uint8_t resetCode) {
  switch (resetCode) {
    case REASON_DEFAULT_RST:   return PSTR("power on");
    case REASON_WDT_RST:       return PSTR("hardware watchdog");
    case REASON_EXCEPTION_RST: return PSTR("crash");
    case REASON_SOFT_WDT_RST:  return PSTR("software watchdog");
    case REASON_EXT_SYS_RST:   return PSTR("reset button");
    default:                   return PSTR("unknown");
  }
}

bool notifyBuildMessage(const NotifyEventData& event, NotifyMessage& msg) {
  setHeadingFor(event, msg);

  char*        body = msg.body;
  const size_t size = sizeof(msg.body);
  uint8_t      i    = event.index;
  int          n    = 0;   // characters the full text needs, as snprintf() reports it
  body[0] = '\0';

  switch (event.type) {
    case NOTIFY_ALARM_OVER:
      if (i >= MAX_FERMENTERS) return false;
      n = snprintf_P(body, size, PSTR("%s: %.2f %c is above the %.1f %c limit."),
                     fermenterName(i), shownTemp(event.a), unitLetter(),
                     shownTemp(event.b), unitLetter());
      break;

    case NOTIFY_ALARM_UNDER:
      if (i >= MAX_FERMENTERS) return false;
      n = snprintf_P(body, size, PSTR("%s: %.2f %c is below the %.1f %c limit."),
                     fermenterName(i), shownTemp(event.a), unitLetter(),
                     shownTemp(event.b), unitLetter());
      break;

    case NOTIFY_ALARM_ENDED:
      if (i >= MAX_FERMENTERS) return false;
      if (event.a > TEMP_VALID_MIN) {
        n = snprintf_P(body, size, PSTR("%s: back within its limits at %.2f %c."),
                       fermenterName(i), shownTemp(event.a), unitLetter());
      } else {
        // The alarm also ends when the fermenter is switched off or its
        // probe stops reading, and then there is no temperature to give
        n = snprintf_P(body, size, PSTR("%s: the alarm has ended (no temperature reading)."),
                       fermenterName(i));
      }
      break;

    case NOTIFY_PROBE_LOST:
      if (i >= MAX_PROBES) return false;
      n = snprintf_P(body, size, PSTR("%s is not responding. Its readings are no longer used."),
                     probeName(i));
      break;

    case NOTIFY_PROBE_BACK:
      if (i >= MAX_PROBES) return false;
      n = snprintf_P(body, size, PSTR("%s is responding again. Its readings are used."), probeName(i));
      break;

    case NOTIFY_ISPINDEL_LOST:
      if (i >= MAX_ISPINDELS) return false;
      n = snprintf_P(body, size, PSTR("%s: no report for %lu min. Its readings are no longer used."),
                     iSpindelName(i), shownMinutes(event.a));
      break;

    case NOTIFY_ISPINDEL_BACK:
      if (i >= MAX_ISPINDELS) return false;
      n = snprintf_P(body, size, PSTR("%s is reporting again. Its readings are used."), iSpindelName(i));
      break;

    case NOTIFY_TILT_LOST:
      if (i >= MAX_TILTS) return false;
      n = snprintf_P(body, size, PSTR("%s Tilt: no reading for %lu min. Its readings are no longer used."),
                     getTiltColourName(i), shownMinutes(event.a));
      break;

    case NOTIFY_TILT_BACK:
      if (i >= MAX_TILTS) return false;
      n = snprintf_P(body, size, PSTR("%s Tilt is reporting again. Its readings are used."),
                     getTiltColourName(i));
      break;

    case NOTIFY_RESTART: {
      char reason[20];   // the longest is "hardware watchdog", 17 characters
      strncpy_P(reason, restartReasonText(event.detail), sizeof(reason) - 1);
      reason[sizeof(reason) - 1] = '\0';
      n = snprintf_P(body, size, PSTR("The controller restarted unexpectedly. Cause: %s."), reason);
      break;
    }

    case NOTIFY_STEP_COMPLETE:
      if (i >= MAX_FERMENTERS) return false;
      n = snprintf_P(body, size, PSTR("%s: step %u is complete. Step %u has started."),
                     fermenterName(i), (unsigned)event.detail, shownStep(event.a));
      break;

    case NOTIFY_PROFILE_FINISHED:
      if (i >= MAX_FERMENTERS) return false;
      n = snprintf_P(body, size, PSTR("%s: the profile has finished."), fermenterName(i));
      break;

    case NOTIFY_UPDATE_AVAILABLE:
      n = snprintf_P(body, size, PSTR("OurBrewbot %s is available. This controller runs %s."),
                     g_updateStatus.latestVersion, FW_VERSION);
      break;

    case NOTIFY_TEST:
      n = snprintf_P(body, size, PSTR("Test notification from your OurBrewbot controller."));
      break;
  }

  // snprintf() returns the length the whole text needs, so a value that
  // reaches the buffer size means the text was cut short
  return n > 0 && (size_t)n < size;
}

// ============================================================
// SENDING
// ============================================================

// POST one message to the notification URL. Returns the HTTP status code, or
// a negative HTTPClient error (no connection, no reply...).
static int postMessage(const NotifyMessage& msg) {
  WiFiClient client;
  HTTPClient http;

  if (!http.begin(client, g_notifyConfig.url)) return HTTPC_ERROR_CONNECTION_FAILED;
  http.setTimeout(NOTIFY_TIMEOUT_MS);

  char priority[2] = { (char)('0' + msg.priority), '\0' };
  http.addHeader(F("Content-Type"), F("text/plain; charset=utf-8"));
  http.addHeader(F("Title"),    msg.title);
  http.addHeader(F("Priority"), priority);
  http.addHeader(F("Tags"),     msg.tags);

  int code = http.POST((const uint8_t*)msg.body, strlen(msg.body));
  http.end();
  return code;
}

static bool isSuccess(int httpCode) {
  return httpCode >= 200 && httpCode < 300;
}

// Log a failed send; `what` says what happens next, as a PSTR() text. The URL
// is left out on purpose: with ntfy the topic name in it is the only thing
// keeping the notifications private.
static void logFailure(const char* title, int code, PGM_P whatNext) {
  char what[28];   // the longest is "trying again in a minute", 24 characters
  strncpy_P(what, whatNext, sizeof(what) - 1);
  what[sizeof(what) - 1] = 0;
  if (code > 0) {
    logMsgL(SYSLOG_WARNING, "[NOTIFY] %s: HTTP %d - %s", title, code, what);
  } else {
    logMsgL(SYSLOG_WARNING, "[NOTIFY] %s: no reply (error %d) - %s", title, code, what);
  }
}

void notifyEvent(NotifyEvent type, uint8_t index, uint8_t detail, float a, float b) {
  if (!g_notifyConfig.enabled) return;
  if ((g_notifyConfig.events & eventGroup(type)) == 0) return;

  if (s_queueCount >= NOTIFY_QUEUE_SIZE) {
    logMsgL(SYSLOG_WARNING, "[NOTIFY] %u notifications already waiting - a new one was dropped",
            (unsigned)s_queueCount);
    return;
  }

  QueuedEvent& slot = s_queue[s_queueCount];
  slot.event.type   = type;
  slot.event.index  = index;
  slot.event.detail = detail;
  slot.event.a      = a;
  slot.event.b      = b;
  slot.queuedAt     = uptimeStamp();
  s_queueCount++;
}

// ============================================================
// ALARMS AND SENSOR PROBLEMS
//
// An alarm or a missing sensor is a "problem" that is either there or not.
// The code that watches it calls in on every check, and the tracking below
// turns that into two notifications:
//  - "started" at once, the first time the problem is seen;
//  - "over" only once it has stayed away for 10 minutes. A temperature
//    hovering at its limit, or a probe with a loose wire, comes and goes
//    every few minutes, and without the wait each flip would be a message.
// ============================================================

// How long a problem must stay away before the "over" message is sent
#define NOTIFY_PROBLEM_OVER_S  600

// What has been sent for a problem that is going on
enum : uint8_t {
  PROBLEM_NOT_SENT    = 0,
  PROBLEM_SENT        = 1,   // "started" has been sent
  PROBLEM_SENT_SEVERE = 2,   // ...and so has the urgent one (severe alarms only)
};

// One tracker for each thing that can have a problem: every fermenter's
// alarm, then every probe, iSpindel and Tilt
#define TRACK_ALARMS     0
#define TRACK_PROBES     (TRACK_ALARMS    + MAX_FERMENTERS)
#define TRACK_ISPINDELS  (TRACK_PROBES    + MAX_PROBES)
#define TRACK_TILTS      (TRACK_ISPINDELS + MAX_ISPINDELS)
#define TRACK_COUNT      (TRACK_TILTS     + MAX_TILTS)

static uint8_t  s_problemSent[TRACK_COUNT]     = {0};
static uint32_t s_problemOffSince[TRACK_COUNT] = {0};   // uptimeStamp() it went away; 0 = it is there

static void forgetProblem(uint8_t tracker) {
  s_problemSent[tracker]     = PROBLEM_NOT_SENT;
  s_problemOffSince[tracker] = 0;
}

// The problem is there. Returns true if "started" should be sent now: the
// first time, and once more if it has become severe.
static bool problemStarted(uint8_t tracker, uint8_t level) {
  s_problemOffSince[tracker] = 0;
  if (level <= s_problemSent[tracker]) return false;
  s_problemSent[tracker] = level;
  return true;
}

// The problem is not there. Returns true if "over" should be sent now: once,
// when a problem that was reported has stayed away for 10 minutes.
static bool problemOver(uint8_t tracker) {
  if (s_problemSent[tracker] == PROBLEM_NOT_SENT) return false;   // nothing was reported
  if (s_problemOffSince[tracker] == 0) s_problemOffSince[tracker] = uptimeStamp();
  if (uptimeSince(s_problemOffSince[tracker]) < NOTIFY_PROBLEM_OVER_S) return false;
  forgetProblem(tracker);
  return true;
}

// True when this kind of event is ticked in the settings
static bool eventsWanted(uint8_t group) {
  return g_notifyConfig.enabled && (g_notifyConfig.events & group) != 0;
}

void notifyAlarmState(uint8_t fermenter, bool active, bool overTemp, bool severe,
                      float temp, float limit) {
  if (fermenter >= MAX_FERMENTERS) return;
  uint8_t tracker = TRACK_ALARMS + fermenter;

  // Alarm notifications are off: forget what was sent, so that switching
  // them on in the middle of an alarm reports that alarm
  if (!eventsWanted(NOTIFY_EVT_ALARMS)) {
    forgetProblem(tracker);
    return;
  }

  if (active) {
    if (problemStarted(tracker, severe ? PROBLEM_SENT_SEVERE : PROBLEM_SENT)) {
      notifyEvent(overTemp ? NOTIFY_ALARM_OVER : NOTIFY_ALARM_UNDER, fermenter, severe ? 1 : 0, temp, limit);
    }
  } else if (problemOver(tracker)) {
    notifyEvent(NOTIFY_ALARM_ENDED, fermenter, 0, temp);
  }
}

void notifySensorState(NotifySensor kind, uint8_t index, bool lost, float minutes) {
  uint8_t     tracker   = 0;
  NotifyEvent lostEvent = NOTIFY_PROBE_LOST;
  NotifyEvent backEvent = NOTIFY_PROBE_BACK;

  switch (kind) {
    case NOTIFY_SENSOR_PROBE:
      if (index >= MAX_PROBES) return;
      tracker = TRACK_PROBES + index;
      break;
    case NOTIFY_SENSOR_ISPINDEL:
      if (index >= MAX_ISPINDELS) return;
      tracker   = TRACK_ISPINDELS + index;
      lostEvent = NOTIFY_ISPINDEL_LOST;
      backEvent = NOTIFY_ISPINDEL_BACK;
      break;
    case NOTIFY_SENSOR_TILT:
      if (index >= MAX_TILTS) return;
      tracker   = TRACK_TILTS + index;
      lostEvent = NOTIFY_TILT_LOST;
      backEvent = NOTIFY_TILT_BACK;
      break;
  }

  if (!eventsWanted(NOTIFY_EVT_SENSORS)) {
    forgetProblem(tracker);
    return;
  }

  if (lost) {
    if (problemStarted(tracker, PROBLEM_SENT)) notifyEvent(lostEvent, index, 0, minutes);
  } else if (problemOver(tracker)) {
    notifyEvent(backEvent, index);
  }
}

bool notifySendDue() {
  if (s_queueCount == 0 || !g_notifyConfig.enabled) return false;
  if (!WiFi.isConnected()) return false;   // waiting for WiFi
  if (s_tries > 0 && uptimeSince(s_lastTryAt) < NOTIFY_RETRY_S) return false;   // waiting to try again
  return true;
}

void notifyLoop() {
  if (s_queueCount == 0) return;

  // Switched off while events were waiting: they are not wanted any more
  if (!g_notifyConfig.enabled) {
    s_queueCount = 0;
    s_tries      = 0;
    return;
  }

  // Too old to be useful (WiFi was down, or the server kept failing)
  if (uptimeSince(s_queue[0].queuedAt) > NOTIFY_MAX_AGE_S) {
    logMsgL(SYSLOG_WARNING, "[NOTIFY] A notification waited over an hour - dropped unsent");
    removeFront();
    return;
  }

  if (!notifySendDue()) return;

  checkpoint(CP_NOTIFY);   // so a crash while sending is logged against it

  if (!notifyBuildMessage(s_queue[0].event, s_message)) {
    // A fermenter or sensor number that doesn't exist. The callers never
    // pass one, so this is only here to stop a bad record blocking the queue.
    removeFront();
    return;
  }

  int code = postMessage(s_message);
  if (isSuccess(code)) {
    logMsg("[NOTIFY] Sent: %s (HTTP %d)", s_message.title, code);
    removeFront();
    return;
  }

  s_tries++;
  s_lastTryAt = uptimeStamp();
  if (s_tries >= NOTIFY_MAX_TRIES) {
    logFailure(s_message.title, code, PSTR("given up"));
    removeFront();
  } else {
    logFailure(s_message.title, code, PSTR("trying again in a minute"));
  }
}

int notifySendTest() {
  NotifyEventData event = { NOTIFY_TEST, 0, 0, 0.0f, 0.0f };
  notifyBuildMessage(event, s_message);
  int code = postMessage(s_message);
  if (isSuccess(code)) {
    logMsg("[NOTIFY] Sent: %s (HTTP %d)", s_message.title, code);
  } else {
    logFailure(s_message.title, code, PSTR("test not delivered"));
  }
  return code;
}
