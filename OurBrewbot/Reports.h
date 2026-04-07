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

// Individual service reporters — svcIndex selects the brew service config slot
void reportBrewfather(uint8_t fermenterIndex, uint8_t svcIndex);
void reportBrewersFriend(uint8_t fermenterIndex, uint8_t svcIndex);

// Test a brew service connection — returns HTTP status code or error
int testBrewService(uint8_t svcIndex);

// Health check — free heap, uptime etc.
void reportHealth();
