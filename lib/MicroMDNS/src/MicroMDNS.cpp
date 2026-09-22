/*
 * MicroMDNS.cpp — minimal mDNS responder: socket, scheduling, and the loop hook
 *
 * See MicroMDNS.h for why this replaces ESP8266mDNS. All packet decoding and
 * encoding lives in MdnsPacket.cpp so it can be unit tested on the host; this
 * file is only the plumbing around it.
 */

#include "MicroMDNS.h"
#include "MdnsPacket.h"
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <stdarg.h>

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
// having to ask. RFC 6762 8.3 asks for at least two, a second apart, and allows
// more only if the gap at least doubles each time - so 1 s, then 2 s.
#define MDNS_ANNOUNCE_COUNT     3
#define MDNS_ANNOUNCE_FIRST_MS  1000
#define MDNS_ANNOUNCE_GAP_MS    1000

// Goodbyes are sent twice: multicast over WiFi gets no link-level retries, and
// about one packet in eight was measured lost on a busy network. lwIP also
// drops a datagram handed over too soon after the previous one, and the
// caller has usually just logged something (another datagram, if syslog is
// on), so every copy waits a little first. The final wait gives the last copy
// time to leave the radio before a restart pulls the plug.
#define MDNS_GOODBYE_COPIES     2
#define MDNS_GOODBYE_GAP_MS     20
#define MDNS_GOODBYE_FLUSH_MS   50

// One packet in, one packet out, both fixed and file-static. These two buffers
// are most of the memory cost of the responder; nothing here ever touches the
// heap. See MdnsPacket.h for how each size was chosen.
static uint8_t s_rx[MDNS_RX_SIZE];
static uint8_t s_tx[MDNS_TX_SIZE];

static WiFiUDP    s_udp;
static MdnsNames  s_names;
static MdnsTxt    s_txt;
static MdnsLogger s_logger        = NULL;
static char       s_hostname[MDNS_MAX_HOST_LEN + 1];
static char       s_service[MDNS_MAX_SERVICE_LEN + 1];   // "" until mdnsAddService()
static char       s_proto[4];                            // "tcp" or "udp"
static uint32_t   s_ipv4          = 0;
static uint16_t   s_port          = 0;
static bool          s_running       = false;
static uint8_t       s_announcesLeft = 0;
static uint32_t      s_announceAt    = 0;
static uint32_t      s_announceGap   = MDNS_ANNOUNCE_GAP_MS;
static MdnsRateLimit s_rateLimit;   // zeroed as a static: nothing sent yet

// Format one line and hand it to the logger, if there is one. The format
// string stays in flash (PSTR) like every other literal on this chip, so a
// silent responder costs no RAM for its messages.
static void logLine(PGM_P fmt, ...) {
  if (s_logger == NULL) return;

  char line[96];
  va_list args;
  va_start(args, fmt);
  vsnprintf_P(line, sizeof(line), fmt, args);
  va_end(args);
  s_logger(line);
}

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

// Rebuild the names we answer for from the host name, the service (if one has
// been added) and the current address.
static void rebuildNames() {
  bool hasService = (s_service[0] != '\0');
  mdnsBuildNames(s_hostname,
                 hasService ? s_service : NULL,
                 hasService ? s_proto   : NULL,
                 s_ipv4, &s_names);
}

// Build the records in plan and put them on the wire, either back to whoever
// asked or out to the whole group.
static void sendResponse(const MdnsQueryPlan& plan, IPAddress dest, uint16_t destPort) {
  // Anything multicast in the last second is left out (RFC 6762 section 6):
  // everyone listening already has it. Direct answers are never limited.
  MdnsQueryPlan toSend = plan;
  uint32_t      now    = millis();
  if (!plan.unicast) {
    toSend.replyMask = mdnsRateLimitFilter(s_rateLimit, plan.replyMask, now);
    if (toSend.replyMask == 0) return;
  }

  size_t len = mdnsBuildResponse(s_tx, sizeof(s_tx), toSend, s_names, s_ipv4,
                                 s_port, s_txt);
  if (len == 0) return;

  int started = plan.unicast
                  ? s_udp.beginPacket(dest, destPort)
                  : s_udp.beginPacketMulticast(MDNS_GROUP, MDNS_PORT,
                                              WiFi.localIP(), MDNS_TTL);
  if (!started) return;

  s_udp.write(s_tx, len);
  if (s_udp.endPacket() && !plan.unicast) {
    mdnsRateLimitRecord(&s_rateLimit, toSend.replyMask, now);
  }
}

// An unsolicited response carrying everything we publish.
static void announce() {
  MdnsQueryPlan plan;
  plan.replyMask = MDNS_REPLY_ALL;
  sendResponse(plan, MDNS_GROUP, MDNS_PORT);
}

// Tell everyone to forget the names we have been answering for: every record
// we could have given out, with a TTL of 0. Sent straight to the group rather
// than through sendResponse(), because the one-second limit must not hold a
// goodbye back - the records may well have been multicast a moment ago.
static void sendGoodbye() {
  MdnsQueryPlan plan;
  plan.replyMask = MDNS_REPLY_ALL | MDNS_REPLY_PTR_REV;
  plan.goodbye   = true;

  size_t len = mdnsBuildResponse(s_tx, sizeof(s_tx), plan, s_names, s_ipv4,
                                 s_port, s_txt);
  if (len == 0) return;

  for (uint8_t copy = 0; copy < MDNS_GOODBYE_COPIES; copy++) {
    delay(MDNS_GOODBYE_GAP_MS);
    if (!s_udp.beginPacketMulticast(MDNS_GROUP, MDNS_PORT, WiFi.localIP(), MDNS_TTL)) continue;
    s_udp.write(s_tx, len);
    s_udp.endPacket();
  }
  delay(MDNS_GOODBYE_FLUSH_MS);
}

static void scheduleAnnouncements() {
  s_announcesLeft = MDNS_ANNOUNCE_COUNT;
  s_announceAt    = millis() + MDNS_ANNOUNCE_FIRST_MS;
  s_announceGap   = MDNS_ANNOUNCE_GAP_MS;
}

void mdnsSetLogger(MdnsLogger logger) {
  s_logger = logger;
}

bool mdnsBegin(const char* hostname) {
  if (!mdnsIsValidHostLabel(hostname)) {
    logLine(PSTR("Host name must be 1-%u letters, digits or hyphens"),
            (unsigned)MDNS_MAX_HOST_LEN);
    return false;
  }

  // Moving to a new name: take the old one off the network first, or caches
  // would keep answering for it until its TTL ran out.
  if (s_running && strcmp(hostname, s_hostname) != 0) {
    logLine(PSTR("Goodbye for %s"), s_names.host);
    sendGoodbye();
  }

  strcpy(s_hostname, hostname);
  s_ipv4 = currentIPv4();
  rebuildNames();

  if (!openSocket()) {
    s_running = false;
    logLine(PSTR("Failed to open the multicast socket"));
    return false;
  }

  s_running = true;
  scheduleAnnouncements();
  logLine(PSTR("Responding for %s"), s_names.host);
  return true;
}

bool mdnsAddService(const char* service, const char* proto, uint16_t port) {
  if (s_service[0] != '\0') return false;                    // only one service
  if (!mdnsIsValidServiceName(service)) return false;
  if (strcmp(proto, "tcp") != 0 && strcmp(proto, "udp") != 0) return false;

  strcpy(s_service, service);
  strcpy(s_proto, proto);
  s_port = port;
  rebuildNames();

  // Already answering: announce again so browsers see the new service now
  // rather than when they next happen to ask.
  if (s_running) scheduleAnnouncements();
  logLine(PSTR("Advertising %s on port %u"), s_names.service, (unsigned)s_port);
  return true;
}

bool mdnsAddTxt(const char* entry) {
  if (!mdnsTxtAdd(&s_txt, entry)) return false;
  if (s_running && s_service[0] != '\0') scheduleAnnouncements();
  return true;
}

void mdnsEnd() {
  if (!s_running) return;

  // With no address, or one we have not announced yet, there is nothing out
  // there to withdraw and no way to send it anyway.
  if (s_ipv4 != 0 && currentIPv4() == s_ipv4) {
    logLine(PSTR("Goodbye for %s"), s_names.host);
    sendGoodbye();
  }

  s_udp.stop();
  s_running       = false;
  s_announcesLeft = 0;
}

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
    rebuildNames();
    scheduleAnnouncements();
    logLine(PSTR("Address changed, re-announcing %s"), s_names.host);
  }

  uint32_t now = millis();
  if (s_announcesLeft > 0 && (int32_t)(now - s_announceAt) >= 0) {
    announce();
    s_announcesLeft--;
    s_announceAt   = now + s_announceGap;
    s_announceGap *= 2;
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
