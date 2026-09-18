/*
 * Mdns.cpp — minimal mDNS responder: socket, scheduling, and the loop hook
 *
 * See Mdns.h for why this replaces ESP8266mDNS. All packet decoding and
 * encoding lives in MdnsPacket.cpp so it can be unit tested on the host; this
 * file is only the plumbing around it.
 */

#include "Mdns.h"
#include "MdnsPacket.h"
#include "Log.h"
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

// The mDNS group address and port, fixed by RFC 6762. Responses go out with a
// hop limit of 255 rather than the usual 1: RFC 6762 section 11 asks for it,
// and some receivers drop packets that arrive with anything else.
static const IPAddress MDNS_GROUP(224, 0, 0, 251);
#define MDNS_PORT  5353
#define MDNS_TTL   255

// At most this many packets are handled per mdnsLoop() call. On a busy network
// there is always another one waiting, and the whole point of this module is
// that no single loop pass can be held up for long. Anything we leave behind
// is dropped by the UDP layer, which only queues a handful of packets anyway,
// and mDNS clients retry.
#define MDNS_PACKETS_PER_PASS  4

// Announce the name a few times on startup so browsers pick it up without
// having to ask (RFC 6762 8.3 asks for at least two, a second apart).
#define MDNS_ANNOUNCE_COUNT     3
#define MDNS_ANNOUNCE_FIRST_MS  1000
#define MDNS_ANNOUNCE_GAP_MS    1000

// One packet in, one packet out, both fixed and file-static. These two buffers
// are the entire memory cost of the responder; nothing here ever touches the
// heap. A query longer than the buffer is read as far as it fits, which is
// safe because the questions come first and compression pointers may only
// point backwards into bytes we already have.
static uint8_t s_rx[512];
static uint8_t s_tx[512];

static WiFiUDP   s_udp;
static MdnsNames s_names;
static char      s_hostname[48];
static uint32_t  s_ipv4          = 0;
static uint16_t  s_httpPort      = 80;
static bool      s_running       = false;
static uint8_t   s_announcesLeft = 0;
static uint32_t  s_announceAt    = 0;

// WiFi.localIP() packs the octets in network order, so read them one at a time
// rather than casting the whole thing - the rest of this module works in host
// order, where the first octet is the most significant byte.
static uint32_t currentIPv4() {
  IPAddress ip = WiFi.localIP();
  return ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
         ((uint32_t)ip[2] << 8)  |  (uint32_t)ip[3];
}

static bool openSocket() {
  s_udp.stop();
  return s_udp.beginMulticast(WiFi.localIP(), MDNS_GROUP, MDNS_PORT) == 1;
}

// Build the records in plan and put them on the wire, either back to whoever
// asked or out to the whole group.
static void sendResponse(const MdnsQueryPlan& plan, IPAddress dest, uint16_t destPort) {
  size_t len = mdnsBuildResponse(s_tx, sizeof(s_tx), plan, s_names, s_ipv4, s_httpPort);
  if (len == 0) return;

  int started = plan.unicast
                  ? s_udp.beginPacket(dest, destPort)
                  : s_udp.beginPacketMulticast(MDNS_GROUP, MDNS_PORT,
                                              WiFi.localIP(), MDNS_TTL);
  if (!started) return;

  s_udp.write(s_tx, len);
  s_udp.endPacket();
}

// An unsolicited response carrying everything we publish.
static void announce() {
  MdnsQueryPlan plan;
  plan.replyMask = MDNS_REPLY_ALL;
  plan.unicast   = false;
  plan.legacy    = false;
  plan.queryId   = 0;
  sendResponse(plan, MDNS_GROUP, MDNS_PORT);
}

static void scheduleAnnouncements() {
  s_announcesLeft = MDNS_ANNOUNCE_COUNT;
  s_announceAt    = millis() + MDNS_ANNOUNCE_FIRST_MS;
}

// Called from setup() in OurBrewbot.cpp - cppcheck's usual cross-file
// blind spot, same as crashLogPendingDeferred() in Crash.cpp.
// cppcheck-suppress unusedFunction
void mdnsBegin(const char* hostname, uint16_t httpPort) {
  strncpy(s_hostname, hostname, sizeof(s_hostname) - 1);
  s_hostname[sizeof(s_hostname) - 1] = '\0';
  s_httpPort = httpPort;
  s_ipv4     = currentIPv4();
  mdnsBuildNames(s_hostname, s_ipv4, &s_names);

  if (!openSocket()) {
    s_running = false;
    logMsg("[MDNS] Failed to open the multicast socket");
    return;
  }

  s_running = true;
  scheduleAnnouncements();
  logMsg("[MDNS] Responding for %s on port %u", s_names.host, (unsigned)s_httpPort);
}

// Called from loop() in OurBrewbot.cpp - same cross-file blind spot.
// cppcheck-suppress unusedFunction
void mdnsLoop() {
  if (!s_running) return;

  // No address means WiFi is down or still getting one; there is nothing to
  // answer with until it comes back.
  uint32_t ip = currentIPv4();
  if (ip == 0) return;

  // A DHCP renewal or a reconnect can move us. Rebuild the names and announce
  // again so nothing is left pointing at the old address.
  // s_ipv4 only moves once the new socket is actually open, so a failure here
  // is retried on the next pass instead of being silently forgotten.
  if (ip != s_ipv4 && openSocket()) {
    s_ipv4 = ip;
    mdnsBuildNames(s_hostname, s_ipv4, &s_names);
    scheduleAnnouncements();
    logMsg("[MDNS] Address changed, re-announcing %s", s_names.host);
  }

  uint32_t now = millis();
  if (s_announcesLeft > 0 && (int32_t)(now - s_announceAt) >= 0) {
    announce();
    s_announcesLeft--;
    s_announceAt = now + MDNS_ANNOUNCE_GAP_MS;
  }

  for (uint8_t i = 0; i < MDNS_PACKETS_PER_PASS; i++) {
    if (s_udp.parsePacket() <= 0) break;

    // Never call s_udp.flush() to discard the tail of an over-long packet: on
    // this core flush() is endPacket(), so it would try to transmit. The next
    // parsePacket() moves on by itself, unread bytes and all.
    int len = s_udp.read(s_rx, sizeof(s_rx));
    if (len <= 0) continue;

    // Our own announcements come back to us on the group address.
    if (s_udp.remoteIP() == WiFi.localIP()) continue;

    MdnsQueryPlan plan;
    if (!mdnsParseQuery(s_rx, (size_t)len, s_names, s_udp.remotePort(), &plan)) continue;
    if (plan.replyMask == 0) continue;

    sendResponse(plan, s_udp.remoteIP(), s_udp.remotePort());
  }
}
