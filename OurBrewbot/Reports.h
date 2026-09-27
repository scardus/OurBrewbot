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
 * Reports.h — Cloud service reporting
 *
 * HTTP brew service integrations:
 *   Brewfather:      http://log.brewfather.net/stream?id=...
 *   Brewer's Friend: http://log.brewersfriend.com/stream/...
 */

#include "Config.h"

// Staggered cloud reporting: the 15-min tick queues eligible service×fermenter
// pairs; the loop drains ONE blocking POST per pass so a slow or unreachable
// service can never stall the loop more than ~5 s.
void queueReports();        // mark every enabled service×fermenter pair pending
bool reportsPending();      // anything still queued? (also the Tilt-scan interlock in 0.3.20)
void processReportQueue();  // send AT MOST one pending report, then return

// Report one fermenter to one brew service slot (0=Brewer's Friend, 1=Brewfather)
void reportBrewService(uint8_t fermenterIndex, uint8_t svcIndex);

// Test a brew service connection — returns HTTP status code or error
int testBrewService(uint8_t svcIndex);
