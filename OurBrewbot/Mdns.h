#pragma once
/*
 * Mdns.h — minimal mDNS responder (replaces ESP8266mDNS / LEAmDNS)
 *
 * Makes the device answer to "<hostname>.local" and advertise its web server
 * as an "_http._tcp" service. That is the whole feature set the firmware ever
 * used from the ESP8266mDNS library.
 *
 * Why this exists rather than the library: LEAmDNS parses inbound packets
 * inside the WiFi stack's receive callback and allocates ~544 bytes of heap
 * per record it reads, which produced 50 hardware-watchdog resets between May
 * and July 2026 on a busy network. MDNS.update() was never where that work
 * happened, so the guards wrapped around it could not help. Here every packet
 * is handled from mdnsLoop() in the main loop, out of fixed buffers, with a
 * hard cap on how many are serviced per pass.
 *
 * Deliberate omissions, all safe for a device named after its own chip ID:
 *   - no probing for name conflicts before claiming the name
 *   - no known-answer suppression (we may repeat an answer someone already had)
 *   - no service discovery client (we never ask, only answer)
 *
 * Usage:
 *   mdnsBegin("ourbrewbot-2924fa", 80);   // once, after WiFi is up
 *   mdnsLoop();                           // every pass of loop()
 */

#include <stdint.h>

// Claim the name and start answering. hostname is the bare label with no
// ".local" suffix; httpPort is advertised in the SRV record. Safe to call
// again to move to a new name.
void mdnsBegin(const char* hostname, uint16_t httpPort);

// Service the responder: send any due announcements and answer queries that
// have arrived. Call every pass of loop(); it returns immediately when there
// is nothing waiting.
void mdnsLoop();

