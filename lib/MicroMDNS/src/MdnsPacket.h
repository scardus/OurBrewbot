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
#define MDNS_REPLY_NSEC      0x40  // NSEC: <host>.local                -> "only an A record exists"

// Everything an announcement sends. A browser that asks for the service gets
// the same set, so it never has to come back for the SRV/TXT/A.
#define MDNS_REPLY_ALL  (MDNS_REPLY_A | MDNS_REPLY_PTR_SVC | MDNS_REPLY_SRV | \
                         MDNS_REPLY_TXT | MDNS_REPLY_PTR_META | MDNS_REPLY_NSEC)

// The records that only exist once a service has been added.
#define MDNS_REPLY_SERVICE  (MDNS_REPLY_PTR_SVC | MDNS_REPLY_SRV | \
                             MDNS_REPLY_TXT | MDNS_REPLY_PTR_META)

// DNS record types we care about.
#define MDNS_TYPE_A    1
#define MDNS_TYPE_PTR  12
#define MDNS_TYPE_TXT  16
#define MDNS_TYPE_AAAA 28
#define MDNS_TYPE_SRV  33
#define MDNS_TYPE_NSEC 47
#define MDNS_TYPE_ANY  255

// RFC 6762 section 6: a record may not be multicast again until this long
// after it was last multicast. Answers sent straight back to the asker are not
// limited.
#define MDNS_MULTICAST_GAP_MS  1000

// Longest name we will decode out of a packet, including the NUL. A DNS name
// tops out at 255 bytes on the wire and shrinks slightly in dotted form.
#define MDNS_MAX_NAME  256

// Size of the buffer an inbound query is read into. A longer query is read as
// far as it fits, which is safe: the questions come first, and the answer
// sections after them are never read.
#define MDNS_RX_SIZE          512

// Size of the buffer a response is built in. The biggest response is one query
// asking for every record we have (the announcement set plus the reverse PTR)
// with the three limits below all at their maximum and an address of
// 255.255.255.255. Raise any limit and a native test fails until this is
// raised too - a response that does not fit is not sent at all.
#define MDNS_TX_SIZE          768

// Longest host label we accept, not counting the NUL. 63 is the DNS limit for
// one label, but the host name is repeated eight times in the biggest
// response, so every character here costs eight bytes of MDNS_TX_SIZE.
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

// What a parsed query wants from us. Every field starts at a safe default, so
// a plan built by hand only needs the fields it cares about.
struct MdnsQueryPlan {
  uint8_t  replyMask = 0;         // 0 = nothing here is ours; send nothing at all
  bool     unicast   = false;     // answer the sender directly instead of the group
  bool     legacy    = false;     // sender wasn't on port 5353, so it's a plain DNS resolver
  uint16_t queryId   = 0;         // echoed back on a legacy answer; 0 otherwise

  // A legacy resolver expects its question repeated in the answer (RFC 6762
  // section 6.7). These hold the first question we could answer: the name
  // points at our own copy in MdnsNames, so it stays valid after the packet
  // buffer is reused. NULL means there is no question to repeat.
  const char* echoName  = nullptr;
  uint16_t    echoType  = 0;
  uint16_t    echoClass = 0;
};

// When each record was last multicast, one slot per MDNS_REPLY_* bit. sentMask
// says which slots hold a real time: millis() starts at 0 after a reboot, so a
// time of 0 cannot also mean "never sent".
struct MdnsRateLimit {
  uint32_t lastSent[8];
  uint8_t  sentMask;
};

// Fill in the names we answer for. hostname is the bare label with no
// ".local" ("ourbrewbot-2924fa"); ipv4 is host byte order. service and proto
// are the bare labels without underscores ("http", "tcp"); pass service as
// NULL to answer for the host name only.
void mdnsBuildNames(const char* hostname, const char* service, const char* proto,
                    uint32_t ipv4, MdnsNames* names);

// True if hostname is a usable host label: 1 to MDNS_MAX_HOST_LEN letters,
// digits and hyphens, not starting or ending with a hyphen (RFC 1123).
bool mdnsIsValidHostLabel(const char* hostname);

// True if service is a usable DNS-SD service name (RFC 6763 section 7.2): 1 to
// 15 letters, digits and hyphens, at least one letter, not starting or ending
// with a hyphen, and no two hyphens in a row.
bool mdnsIsValidServiceName(const char* service);

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

// Of the records in mask, return only those that may be multicast at time now
// (milliseconds): anything multicast less than MDNS_MULTICAST_GAP_MS ago is
// dropped. Safe across millis() wrapping.
uint8_t mdnsRateLimitFilter(const MdnsRateLimit& limit, uint8_t mask, uint32_t now);

// Note that the records in mask were multicast at time now.
void mdnsRateLimitRecord(MdnsRateLimit* limit, uint8_t mask, uint32_t now);

// Build the response described by plan into out[0..outSize). Returns the
// number of bytes written, or 0 if there was nothing to send or it wouldn't
// fit. ipv4 is host byte order; port goes in the SRV record and txt in the
// TXT record. Service records are left out when names has no service, and the
// NSEC record is left out for a legacy resolver, which does not expect one.
size_t mdnsBuildResponse(uint8_t* out, size_t outSize, const MdnsQueryPlan& plan,
                         const MdnsNames& names, uint32_t ipv4, uint16_t port,
                         const MdnsTxt& txt);
