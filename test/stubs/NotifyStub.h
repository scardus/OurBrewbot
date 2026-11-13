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
// Stand-in for Notify.cpp, for the suites that test the code which REPORTS
// events (Fermenter.cpp, Temperatures.cpp, Profile.cpp...) rather than the
// notifications themselves. Nothing is queued or sent: each call is recorded
// so a test can check that the right event was reported, with the right
// values. Notify.cpp's own behaviour is covered by test_native_notify.
//
// Call notifyStubReset() from setUp().

#include "../../OurBrewbot/Notify.h"

// The most recent notifyEvent() call, and how many there have been
struct NotifyStubEvent {
  int             count;
  NotifyEventData last;
};

// The most recent notifySensorState() call for each sensor, and how many
// there have been for it. Indexed [kind][index]; 8 covers every kind.
struct NotifyStubSensor {
  int   count;
  bool  lost;
  float minutes;
};

// The most recent notifyAlarmState() call for each fermenter
struct NotifyStubAlarm {
  int   count;
  bool  active;
  bool  overTemp;
  bool  severe;
  float temp;
  float limit;
};

static NotifyStubEvent g_notifyStubEvent;
static NotifyStubAlarm g_notifyStubAlarm[4];   // one per fermenter (MAX_FERMENTERS)
static NotifyStubSensor g_notifyStubSensor[3][8];
static int g_notifyStubProfileStarts = 0;   // notifyProfileStarted() calls

static void notifyStubReset() {
  g_notifyStubEvent = NotifyStubEvent{};
  g_notifyStubProfileStarts = 0;
  for (int i = 0; i < 4; i++) g_notifyStubAlarm[i] = NotifyStubAlarm{};
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < 8; i++) g_notifyStubSensor[k][i] = NotifyStubSensor{};
  }
}

void notifySensorState(NotifySensor kind, uint8_t index, bool lost, float minutes) {
  if (kind > NOTIFY_SENSOR_TILT || index >= 8) return;
  NotifyStubSensor& sensor = g_notifyStubSensor[kind][index];
  sensor.count++;
  sensor.lost    = lost;
  sensor.minutes = minutes;
}

void notifyEvent(NotifyEvent type, uint8_t index, uint8_t detail, float a, float b) {
  g_notifyStubEvent.count++;
  g_notifyStubEvent.last = NotifyEventData{ type, index, detail, a, b };
}

// Recorded as the event it queues, so a test checks it the same way
void notifyProfileFinished(uint8_t fermenter) {
  notifyEvent(NOTIFY_PROFILE_FINISHED, fermenter);
}

void notifyProfileStarted(uint8_t) {
  g_notifyStubProfileStarts++;
}

void notifyAlarmState(uint8_t fermenter, bool active, bool overTemp, bool severe,
                      float temp, float limit) {
  if (fermenter >= 4) return;
  NotifyStubAlarm& alarm = g_notifyStubAlarm[fermenter];
  alarm.count++;
  alarm.active   = active;
  alarm.overTemp = overTemp;
  alarm.severe   = severe;
  alarm.temp     = temp;
  alarm.limit    = limit;
}
