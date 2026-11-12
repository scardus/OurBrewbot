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
 * Notify.h — Push notifications over plain HTTP
 *
 * When something happens that the brewer should know about (a temperature
 * alarm, a sensor that stopped reporting, an unexpected restart...), the
 * controller POSTs a short text message to the URL in NotifyConfig. The
 * request is the one the ntfy service expects (https://ntfy.sh, or a copy of
 * it running on the home network):
 *
 *   POST http://ntfy.sh/my-topic
 *   Title: Temperature alarm
 *   Priority: 4
 *   Tags: warning
 *
 *   Fermenter 1: 29.2 C is above the 29.0 C limit.
 *
 * Anything else that accepts a plain POST (a Home Assistant webhook, for
 * example) gets the same request and can ignore the headers.
 *
 * Only http:// is supported. https:// needs about 20 KB of memory for the
 * encryption, which this controller does not have to spare.
 *
 * Sending is split in two so a slow server can't hold up fermenter control:
 *   notifyEvent()  is called where the event happens. It only adds a small
 *                  record to a queue.
 *   notifyLoop()   is called from loop(). It sends AT MOST one queued
 *                  notification per pass.
 */

#include <stdint.h>
#include <stddef.h>

// What happened. The comment on each says what its `index`, `detail`, `a`
// and `b` values mean - anything not mentioned is unused (pass 0).
enum NotifyEvent : uint8_t {
  NOTIFY_ALARM_OVER,        // index = fermenter, detail = 1 if severe, a = temperature, b = ceiling (both in C)
  NOTIFY_ALARM_UNDER,       // index = fermenter, detail = 1 if severe, a = temperature, b = floor (both in C)
  NOTIFY_ALARM_ENDED,       // index = fermenter, a = temperature in C (TEMP_NONE when there is no reading)
  NOTIFY_PROBE_LOST,        // index = probe
  NOTIFY_PROBE_BACK,        // index = probe
  NOTIFY_ISPINDEL_LOST,     // index = iSpindel slot, a = minutes without a report
  NOTIFY_ISPINDEL_BACK,     // index = iSpindel slot
  NOTIFY_TILT_LOST,         // index = Tilt colour, a = minutes without a reading
  NOTIFY_TILT_BACK,         // index = Tilt colour
  NOTIFY_RESTART,           // detail = the reset reason code (REASON_xxx in user_interface.h)
  NOTIFY_STEP_COMPLETE,     // index = fermenter, detail = the step that finished, a = the step now running
  NOTIFY_PROFILE_FINISHED,  // index = fermenter
  NOTIFY_UPDATE_AVAILABLE,  // the version is read from g_updateStatus when the message is sent
  NOTIFY_TEST,              // the test message from the WebUI (never queued)
};

// One event, as it is kept in the queue. The text is only built when the
// notification is sent, so a waiting event costs 12 bytes rather than 128.
struct NotifyEventData {
  NotifyEvent type;
  uint8_t     index;
  uint8_t     detail;
  float       a;
  float       b;
};

// Room for the longest message body. The worst case is worked out in
// test_native_notify, which fails if any message no longer fits.
#define NOTIFY_BODY_SIZE 128

// A notification ready to send
struct NotifyMessage {
  char    title[40];                // fixed ASCII text: it travels as an HTTP header
  char    tags[20];                 // ntfy tag name, shown as an emoji on the phone
  uint8_t priority;                 // ntfy priority: 1 (lowest) to 5 (highest), 3 = normal
  char    body[NOTIFY_BODY_SIZE];   // the message; may hold names typed by the user
};

// Queue a notification. Returns straight away without sending anything.
// Nothing is queued when notifications are off, or when the event's kind is
// not ticked in the settings (NotifyConfig.events).
void notifyEvent(NotifyEvent type, uint8_t index = 0, uint8_t detail = 0,
                 float a = 0.0f, float b = 0.0f);

// Call every loop pass. Sends AT MOST one waiting notification, then returns;
// with nothing to send it returns straight away. The POST blocks for up to
// about 15 s if the server does not answer (see NOTIFY_TIMEOUT_MS). A failed
// one is tried again a minute later, three times in all.
void notifyLoop();

// Tell the notifications what a fermenter's temperature alarm is doing. Call
// it on every alarm check, whether or not anything changed; this works out
// what to send:
//  - "alarm started" at once, the first time the alarm is seen active (and
//    again, as urgent, if a mild alarm later becomes severe);
//  - "alarm ended" only once the alarm has stayed off for 10 minutes. A
//    temperature hovering at its limit switches the alarm on and off every
//    few minutes, and without the wait each flip would be a notification.
// temp and limit are in Celsius; limit is the ceiling or floor that was
// passed. With the alarm off, temp is the current reading, or TEMP_NONE.
void notifyAlarmState(uint8_t fermenter, bool active, bool overTemp, bool severe,
                      float temp, float limit);

// The kinds of sensor notifySensorState() follows
enum NotifySensor : uint8_t {
  NOTIFY_SENSOR_PROBE,      // index = probe
  NOTIFY_SENSOR_ISPINDEL,   // index = iSpindel slot
  NOTIFY_SENSOR_TILT,       // index = Tilt colour
};

// Tell the notifications whether a sensor is reporting. Like
// notifyAlarmState(), call it on every check: "stopped reporting" is sent at
// once, and "reporting again" only once the sensor has kept reporting for 10
// minutes, so a loose connection is one message and not dozens. `minutes` is
// how long the sensor has been quiet (not used for probes).
void notifySensorState(NotifySensor kind, uint8_t index, bool lost, float minutes = 0.0f);

// True when the next notifyLoop() call will send something, and so may block
// the loop for a few seconds. The Tilt scan checks this, as it does for brew
// service reports, so it doesn't start a scan that would be starved.
bool notifySendDue();

// Send the test message now, whatever the settings tick boxes say. Returns
// the HTTP status code, or a negative HTTPClient error.
int notifySendTest();

// Build the title, priority and text for an event. Returns false if the event
// names a fermenter or sensor that doesn't exist, or the text did not fit.
// Only called from this module and its tests.
bool notifyBuildMessage(const NotifyEventData& event, NotifyMessage& msg);
