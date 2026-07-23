#pragma once
/*
 * Reports.h — Cloud service reporting
 *
 * HTTP brew service integrations:
 *   Brewfather:      http://log.brewfather.net/stream?id=...
 *   Brewer's Friend: http://log.brewersfriend.com/stream/...
 */

#include "Config.h"

// Send all configured reports
void sendReports();

// Report one fermenter to one brew service slot (0=Brewer's Friend, 1=Brewfather)
void reportBrewService(uint8_t fermenterIndex, uint8_t svcIndex);

// Test a brew service connection — returns HTTP status code or error
int testBrewService(uint8_t svcIndex);
