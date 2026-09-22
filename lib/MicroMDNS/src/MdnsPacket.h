#pragma once
/*
 * MdnsPacket.h — mDNS query parsing and response building (no networking)
 *
 * Kept apart from MicroMDNS.cpp so it can be unit tested on the host without
 * WiFi: everything in here works on plain byte buffers.
 *
 * Scope is deliberately tiny. We answer for exactly one host name and at most
 * one DNS-SD service. There is no known-answer suppression, no probing, no
 * discovery client, and no heap use anywhere in this file — every buffer is
 * caller-supplied and every write is bounds checked.
 *
 * Wire format: RFC 1035 (DNS messages), RFC 6762 (mDNS), RFC 6763 (DNS-SD).
 */

#include <stdint.h>
#include <stddef.h>

// Which records a query asked us for; OR'd together into MdnsQueryPlan::replyMask.
#define MDNS_REPLY_A         0x01  // A:   <host>.local                 -> our IPv4
#define MDNS_REPLY_PTR_SVC   0x02  // PTR: _<svc>._<proto>.local        -> <instance>
#define MDNS_REPLY_SRV       0x04  // SRV: <instance>                   -> <host>.local:<port>
#define MDNS_REPLY_TXT       0x08  // TXT: <instance>                   -> the TXT entries
#define MDNS_REPLY_PTR_META  0x10  // PTR: _services._dns-sd._udp.local -> _<svc>._<proto>.local
#define MDNS_REPLY_PTR_REV   0x20  // PTR: <ip>.in-addr.arpa            -> <host>.local

// Everything an announcement sends. A browser that asks for the service gets
// the same set, so it never has to come back for the SRV/TXT/A.
#define MDNS_REPLY_ALL  (MDNS_REPLY_A | MDNS_REPLY_PTR_SVC | MDNS_REPLY_SRV | \
                         MDNS_REPLY_TXT | MDNS_REPLY_PTR_META)

// The records that only exist once a service has been added.
#define MDNS_REPLY_SERVICE  (MDNS_REPLY_PTR_SVC | MDNS_REPLY_SRV | \
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

// Size of the buffer a full response is built in. The three limits below are
// chosen together so that an announcement at its very largest - longest host,
// longest service name, full TXT record - still fits: that works out at 491
// bytes. Raise any of them and a native test fails until this is raised too.
#define MDNS_PACKET_SIZE      512

// Longest host label we accept, not counting the NUL. 63 is the DNS limit for
// one label, but the host name is repeated five times in a full announcement,
// so every character here costs five bytes of MDNS_PACKET_SIZE.
#define MDNS_MAX_HOST_LEN     32

// RFC 6763 section 7.2: a service name is at most 15 characters.
#define MDNS_MAX_SERVICE_LEN  15

// Room for the TXT record's entries, in wire form. Each entry costs its length
// plus one byte.
#define MDNS_MAX_TXT_LEN      64

// The names we answer for, built by mdnsBuildNames(). Stored as lowercase
// dotted strings ("ourbrewbot-2924fa.local") because comparing those is far
// easier to follow than comparing length-prefixed wire labels.
struct MdnsNames {
  char host[64];      // "ourbrewbot-2924fa.local"
  char service[32];   // "_http._tcp.local", or "" when there is no service
  char instance[96];  // "ourbrewbot-2924fa._http._tcp.local", or ""
  char reverse[32];   // "207.0.168.192.in-addr.arpa"
  bool hasService;    // false until a service is added: we then answer for the host only
};

// The service's TXT record, already in wire form: each entry is one length
// byte followed by that many bytes of "key=value".
struct MdnsTxt {
  uint8_t data[MDNS_MAX_TXT_LEN];
  uint8_t len;        // 0 = no entries; an empty TXT record is sent instead
};

// What a parsed query wants from us.
struct MdnsQueryPlan {
  uint8_t  replyMask;  // 0 = nothing here is ours; send nothing at all
  bool     unicast;    // answer the sender directly instead of the group
  bool     legacy;     // sender wasn't on port 5353, so it's a plain DNS resolver
  uint16_t queryId;    // echoed back on a legacy answer; 0 otherwise
};

// Fill in the names we answer for. hostname is the bare label with no
// ".local" ("ourbrewbot-2924fa"); ipv4 is host byte order. service and proto
// are the bare labels without underscores ("http", "tcp"); pass service as
// NULL to answer for the host name only.
void mdnsBuildNames(const char* hostname, const char* service, const char* proto,
                    uint32_t ipv4, MdnsNames* names);

// Append one "key=value" entry to txt. Returns false, leaving txt unchanged,
// if the entry is empty or does not fit in the space that is left.
bool mdnsTxtAdd(MdnsTxt* txt, const char* entry);

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
// fit. ipv4 is host byte order; port goes in the SRV record and txt in the
// TXT record. Service records are left out when names has no service.
size_t mdnsBuildResponse(uint8_t* out, size_t outSize, const MdnsQueryPlan& plan,
                         const MdnsNames& names, uint32_t ipv4, uint16_t port,
                         const MdnsTxt& txt);
