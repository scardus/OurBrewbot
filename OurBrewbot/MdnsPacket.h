#pragma once
/*
 * MdnsPacket.h — mDNS query parsing and response building (no networking)
 *
 * Pulled out of Mdns.cpp for the same reason MqttParse.cpp was pulled out of
 * Mqtt.cpp: everything in here works on plain byte buffers, so it can be unit
 * tested on the host without WiFi.
 *
 * Scope is deliberately tiny. We answer for exactly one host name and one
 * "_http._tcp" service, which is all the firmware ever registered with the
 * ESP8266mDNS (LEAmDNS) library this replaces. There is no known-answer
 * suppression, no probing, no discovery client, and no heap use anywhere in
 * this file — every buffer is caller-supplied and every write is bounds
 * checked.
 *
 * Wire format: RFC 1035 (DNS messages), RFC 6762 (mDNS), RFC 6763 (DNS-SD).
 */

#include <stdint.h>
#include <stddef.h>

// Which records a query asked us for; OR'd together into MdnsQueryPlan::replyMask.
#define MDNS_REPLY_A         0x01  // A:   <host>.local                 -> our IPv4
#define MDNS_REPLY_PTR_SVC   0x02  // PTR: _http._tcp.local             -> <instance>
#define MDNS_REPLY_SRV       0x04  // SRV: <instance>                   -> <host>.local:<port>
#define MDNS_REPLY_TXT       0x08  // TXT: <instance>                   -> (empty)
#define MDNS_REPLY_PTR_META  0x10  // PTR: _services._dns-sd._udp.local -> _http._tcp.local
#define MDNS_REPLY_PTR_REV   0x20  // PTR: <ip>.in-addr.arpa            -> <host>.local

// Everything an announcement sends. A browser that asks for the service gets
// the same set, so it never has to come back for the SRV/TXT/A.
#define MDNS_REPLY_ALL  (MDNS_REPLY_A | MDNS_REPLY_PTR_SVC | MDNS_REPLY_SRV | \
                         MDNS_REPLY_TXT | MDNS_REPLY_PTR_META)

// DNS record types we care about.
#define MDNS_TYPE_A    1
#define MDNS_TYPE_PTR  12
#define MDNS_TYPE_TXT  16
#define MDNS_TYPE_SRV  33
#define MDNS_TYPE_ANY  255

// Longest name we will decode out of a packet, including the NUL. A DNS name
// tops out at 255 bytes on the wire and shrinks slightly in dotted form.
#define MDNS_MAX_NAME  256

// The names we answer for, built once at startup by mdnsBuildNames(). Stored
// as lowercase dotted strings ("ourbrewbot-2924fa.local") because comparing
// those is far easier to follow than comparing length-prefixed wire labels.
struct MdnsNames {
  char host[64];      // "ourbrewbot-2924fa.local"
  char service[32];   // "_http._tcp.local"
  char instance[96];  // "ourbrewbot-2924fa._http._tcp.local"
  char reverse[32];   // "207.0.168.192.in-addr.arpa"
};

// What a parsed query wants from us.
struct MdnsQueryPlan {
  uint8_t  replyMask;  // 0 = nothing here is ours; send nothing at all
  bool     unicast;    // answer the sender directly instead of the group
  bool     legacy;     // sender wasn't on port 5353, so it's a plain DNS resolver
  uint16_t queryId;    // echoed back on a legacy answer; 0 otherwise
};

// Fill in the four names we answer for. hostname is the bare label with no
// ".local" ("ourbrewbot-2924fa"); ipv4 is host byte order.
void mdnsBuildNames(const char* hostname, uint32_t ipv4, MdnsNames* names);

// Decode the DNS name starting at pkt[pos] into a lowercase dotted string.
// Follows compression pointers, which must point strictly backwards (that is
// what makes the loop terminate). On success *nextPos is the offset of the
// first byte after the name as it appeared at pos. Returns false on a
// malformed or over-long name, leaving out[] unusable.
bool mdnsDecodeName(const uint8_t* pkt, size_t len, size_t pos,
                    char* out, size_t outSize, size_t* nextPos);

// Work out what, if anything, the query in pkt[0..len) asks us to answer.
// srcPort is the UDP source port, used to spot legacy (non-5353) resolvers.
// Returns false if the packet isn't a query we should act on; plan is filled
// in either way, with replyMask 0 when there is nothing to say.
bool mdnsParseQuery(const uint8_t* pkt, size_t len, const MdnsNames& names,
                    uint16_t srcPort, MdnsQueryPlan* plan);

// Build the response described by plan into out[0..outSize). Returns the
// number of bytes written, or 0 if there was nothing to send or it wouldn't
// fit. ipv4 is host byte order; httpPort goes in the SRV record.
size_t mdnsBuildResponse(uint8_t* out, size_t outSize, const MdnsQueryPlan& plan,
                         const MdnsNames& names, uint32_t ipv4, uint16_t httpPort);
