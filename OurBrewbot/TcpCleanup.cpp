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
 * TcpCleanup.cpp — Clear out closed TCP connections waiting in TIME_WAIT.
 *
 * Why: the ESP8266 core (3.1.2) patches lwIP to keep at most 5 connections
 * in TIME_WAIT (tools/sdk/lwip2/builder/patches/time-wait.patch). When a 6th
 * arrives, it aborts the one that has been idle longest - and that can be
 * the very connection lwIP is still processing, because lwIP stops updating
 * a connection's idle timer once we have closed it. lwIP then carries on
 * using the freed connection and aborts it a second time: a double free that
 * corrupts the heap and crashes the controller later, somewhere unrelated.
 * It happens when a browser re-uses a connection we already closed while
 * more than 5 others are in TIME_WAIT (found 2026-09-27 with FreeTrace).
 *
 * The fix: remove every TIME_WAIT connection on each loop() pass, so there
 * are never 6 and the core's patch never has to pick one. TIME_WAIT only
 * guards against stray packets from an old connection being mistaken for a
 * new one with the same addresses and ports, which doesn't matter on a home
 * network. Removing them this way is safe: they have no callbacks left,
 * nothing else points at them, and tcp_abort() on a TIME_WAIT connection
 * just unlinks and frees it without sending anything.
 */

#include "TcpCleanup.h"
#include <lwip/tcp.h>
#include <lwip/priv/tcp_priv.h>   // tcp_tw_pcbs - the list of TIME_WAIT connections

static uint32_t s_cleared = 0;

// Called from loop() in OurBrewbot.cpp - cppcheck doesn't follow the call
// cppcheck-suppress unusedFunction
uint32_t tcpClearTimeWait() {
  uint32_t count = 0;
  while (tcp_tw_pcbs != nullptr) {
    tcp_abort(tcp_tw_pcbs);   // removes it from tcp_tw_pcbs
    count++;
  }
  s_cleared += count;
  return count;
}

// Called from onTenMinuteTimer() in OurBrewbot.cpp
// cppcheck-suppress unusedFunction
uint32_t tcpTakeClearedCount() {
  uint32_t count = s_cleared;
  s_cleared = 0;
  return count;
}
