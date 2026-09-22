#pragma once
/*
 * MicroMDNS.h — minimal mDNS responder for the ESP8266
 *
 * Makes the device answer to "<hostname>.local" and, optionally, advertise one
 * DNS-SD service such as a web server ("_http._tcp").
 *
 * Why this exists rather than the core's ESP8266mDNS (LEAmDNS): that library
 * parses inbound packets inside the WiFi stack's receive callback and allocates
 * ~544 bytes of heap per record it reads, which produced 50 hardware-watchdog
 * resets between May and July 2026 on a busy network. Here every packet is
 * handled from mdnsLoop() in the main loop, out of fixed buffers, with a hard
 * cap on how many are serviced per pass. Nothing touches the heap.
 *
 * Limitations - all deliberate, to keep it small:
 *   - One host name and at most one service.
 *   - No probing for name conflicts: the name you give must already be unique
 *     on the network. Adding the chip ID is the easy way to do that, e.g.
 *     "mydevice-" + String(ESP.getChipId(), HEX).
 *   - No known-answer suppression (we may repeat an answer someone already had).
 *   - No service discovery client (we never ask, only answer).
 *   - Answers are sent straight away, without the short random delay and
 *     one-per-second repeat limit RFC 6762 recommends.
 *   - Station (STA) interface only: nothing is answered on the soft-AP.
 *   - IPv4 only.
 *
 * Usage:
 *   mdnsBegin("mydevice-2924fa");            // once, after WiFi is up
 *   mdnsAddService("http", "tcp", 80);       // optional
 *   mdnsAddTxt("path=/");                    // optional, repeat for more entries
 *   mdnsLoop();                              // every pass of loop()
 */

#include <stdint.h>

// Receives one finished log line, with no trailing newline. Pass one to
// mdnsSetLogger() to see what the responder is doing; by default it is silent.
typedef void (*MdnsLogger)(const char* message);

// Send the responder's log lines to logger, or NULL to silence them again.
void mdnsSetLogger(MdnsLogger logger);

// Claim the name and start answering. hostname is the bare label with no
// ".local" suffix, at most 32 characters. Returns false if the name is empty or
// too long, or the socket could not be opened. Safe to call again to move to
// a new name; any service already added is kept.
bool mdnsBegin(const char* hostname);

// Advertise one service, e.g. ("http", "tcp", 80) for a web server. service is
// at most 15 characters and proto must be "tcp" or "udp", both without the
// leading underscore. Returns false if either is invalid or a service has
// already been added - only one is supported. May be called before or after
// mdnsBegin().
bool mdnsAddService(const char* service, const char* proto, uint16_t port);

// Add one "key=value" entry to the service's TXT record. Returns false if the
// entry is empty or there is no room left (64 bytes in all, counting one extra
// byte per entry). Without any entries an empty TXT record is sent, as DNS-SD
// requires.
bool mdnsAddTxt(const char* entry);

// Service the responder: send any due announcements and answer queries that
// have arrived. Call every pass of loop(); it returns immediately when there
// is nothing waiting.
void mdnsLoop();
