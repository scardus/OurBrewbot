/*
 * MdnsPacket.cpp — mDNS query parsing and response building
 *
 * See MdnsPacket.h. Nothing in this file touches WiFi, the heap, or any
 * Arduino type, which is what lets the native tests drive it with plain byte
 * arrays.
 *
 * The one structural decision worth knowing about: we never read the answer
 * sections of an inbound query. The library this replaces parsed and
 * heap-allocated every record in them (~544 bytes each) to implement
 * known-answer suppression, and did it inside the WiFi stack's receive
 * callback — that is what produced 50 hardware-watchdog resets between May and
 * July 2026. Skipping suppression costs us a few redundant answers on a busy
 * network and removes the failure mode entirely.
 */

#include "MdnsPacket.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// Fixed parts of the wire format.
#define MDNS_HEADER_LEN   12
#define MDNS_PORT         5353
#define MDNS_CLASS_IN     1
#define MDNS_CLASS_ANY    255
#define MDNS_CACHE_FLUSH  0x8000

// Record lifetimes, in seconds. RFC 6762 section 10 recommends 120 s for
// records tied to a host name and 75 min for the rest; a legacy resolver gets
// 10 s because it never sees our goodbye packets.
#define MDNS_HOST_TTL     120
#define MDNS_SERVICE_TTL  4500
#define MDNS_LEGACY_TTL   10

// A name may redirect through compression pointers this many times before we
// call the packet malformed. Pointers must also point strictly backwards, so
// this is a belt-and-braces limit rather than the thing preventing a loop.
#define MDNS_MAX_POINTER_HOPS  8

// The name every DNS-SD browser asks for to enumerate service types.
#define MDNS_META_QUERY  "_services._dns-sd._udp.local"

// Big enough for any name we build, once encoded to wire form.
#define MDNS_RDATA_MAX   128

// ---- reading ----

static uint16_t readU16(const uint8_t* pkt, size_t pos) {
  return (uint16_t)((pkt[pos] << 8) | pkt[pos + 1]);
}

bool mdnsDecodeName(const uint8_t* pkt, size_t len, size_t pos,
                    char* out, size_t outSize, size_t* nextPos) {
  size_t outLen = 0;
  bool   jumped = false;
  int    hops   = 0;

  if (outSize == 0) return false;
  out[0] = '\0';

  while (true) {
    if (pos >= len) return false;
    uint8_t labelLen = pkt[pos];

    if ((labelLen & 0xC0) == 0xC0) {
      // Compression pointer: two bytes holding a 14-bit offset.
      if (pos + 1 >= len) return false;
      size_t target = ((size_t)(labelLen & 0x3F) << 8) | pkt[pos + 1];
      if (target >= pos) return false;            // forward or self pointers would loop
      if (++hops > MDNS_MAX_POINTER_HOPS) return false;
      if (!jumped) {
        *nextPos = pos + 2;                       // the name ends here in the real stream
        jumped   = true;
      }
      pos = target;
      continue;
    }
    if (labelLen & 0xC0) return false;            // reserved label type

    if (labelLen == 0) {
      if (!jumped) *nextPos = pos + 1;
      out[outLen] = '\0';
      return true;
    }
    if (pos + 1 + labelLen > len) return false;

    if (outLen != 0) {
      if (outLen + 1 >= outSize) return false;
      out[outLen++] = '.';
    }
    if (outLen + labelLen >= outSize) return false;
    for (uint8_t i = 0; i < labelLen; i++) {
      out[outLen++] = (char)tolower((unsigned char)pkt[pos + 1 + i]);
    }
    pos += 1 + labelLen;
  }
}

// ---- names ----

static void toLowerInPlace(char* s) {
  for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

void mdnsBuildNames(const char* hostname, const char* service, const char* proto,
                    uint32_t ipv4, MdnsNames* names) {
  memset(names, 0, sizeof(*names));

  snprintf(names->host, sizeof(names->host), "%s.local", hostname);

  // With no service the service and instance names stay empty, and
  // hasService keeps them from ever being matched against a query.
  if (service != NULL && proto != NULL) {
    snprintf(names->service,  sizeof(names->service),  "_%s._%s.local", service, proto);
    snprintf(names->instance, sizeof(names->instance), "%s._%s._%s.local",
             hostname, service, proto);
    names->hasService = true;
  }

  // in-addr.arpa lists the octets backwards: 192.168.0.207 becomes
  // 207.0.168.192.in-addr.arpa.
  snprintf(names->reverse, sizeof(names->reverse), "%u.%u.%u.%u.in-addr.arpa",
           (unsigned)(ipv4 & 0xFF),         (unsigned)((ipv4 >> 8) & 0xFF),
           (unsigned)((ipv4 >> 16) & 0xFF), (unsigned)((ipv4 >> 24) & 0xFF));

  // Names decoded out of a packet are lowercased, so lowercase ours too and
  // every comparison stays a plain strcmp.
  toLowerInPlace(names->host);
  toLowerInPlace(names->service);
  toLowerInPlace(names->instance);
}

bool mdnsTxtAdd(MdnsTxt* txt, const char* entry) {
  size_t entryLen = strlen(entry);

  // Each entry goes on the wire as one length byte and then the text.
  if (entryLen == 0) return false;
  if (txt->len + 1 + entryLen > sizeof(txt->data)) return false;

  txt->data[txt->len] = (uint8_t)entryLen;
  memcpy(txt->data + txt->len + 1, entry, entryLen);
  txt->len = (uint8_t)(txt->len + 1 + entryLen);
  return true;
}

// ---- parsing ----

// Which of our records does this one question ask for? name is already
// lowercase, as is everything in names.
static uint8_t replyMaskForQuestion(const char* name, uint16_t qType,
                                    const MdnsNames& names) {
  bool    any  = (qType == MDNS_TYPE_ANY);
  uint8_t mask = 0;

  // The service names are only checked when there is a service: with none
  // they are empty strings, and a query for the root name decodes to "" too.
  if (strcmp(name, names.host) == 0) {
    if (any || qType == MDNS_TYPE_A)   mask |= MDNS_REPLY_A;
  } else if (names.hasService && strcmp(name, names.instance) == 0) {
    if (any || qType == MDNS_TYPE_SRV) mask |= MDNS_REPLY_SRV;
    if (any || qType == MDNS_TYPE_TXT) mask |= MDNS_REPLY_TXT;
  } else if (names.hasService && strcmp(name, names.service) == 0) {
    if (any || qType == MDNS_TYPE_PTR) mask |= MDNS_REPLY_PTR_SVC;
  } else if (names.hasService && strcmp(name, MDNS_META_QUERY) == 0) {
    if (any || qType == MDNS_TYPE_PTR) mask |= MDNS_REPLY_PTR_META;
  } else if (strcmp(name, names.reverse) == 0) {
    if (any || qType == MDNS_TYPE_PTR) mask |= MDNS_REPLY_PTR_REV;
  }
  return mask;
}

bool mdnsParseQuery(const uint8_t* pkt, size_t len, const MdnsNames& names,
                    uint16_t srcPort, MdnsQueryPlan* plan) {
  plan->replyMask = 0;
  plan->unicast   = false;
  plan->legacy    = false;
  plan->queryId   = 0;

  if (len < MDNS_HEADER_LEN) return false;

  uint16_t id      = readU16(pkt, 0);
  uint16_t flags   = readU16(pkt, 2);
  uint16_t qdCount = readU16(pkt, 4);

  if (flags & 0x8000)       return false;   // a response, not a question
  if ((flags >> 11) & 0x0F) return false;   // an opcode other than a standard query
  if (qdCount == 0)         return false;

  // Anything not coming from port 5353 is an ordinary DNS resolver that
  // happened to ask on the multicast address. It wants its own query ID back
  // and has no idea what a cache-flush bit means.
  if (srcPort != MDNS_PORT) {
    plan->legacy  = true;
    plan->unicast = true;
    plan->queryId = id;
  }

  size_t pos = MDNS_HEADER_LEN;
  char   name[MDNS_MAX_NAME];

  for (uint16_t q = 0; q < qdCount; q++) {
    size_t next = 0;
    if (!mdnsDecodeName(pkt, len, pos, name, sizeof(name), &next)) break;
    if (next + 4 > len) break;

    uint16_t qType  = readU16(pkt, next);
    uint16_t qClass = readU16(pkt, next + 2);
    pos = next + 4;

    // Top bit of QCLASS is the mDNS "answer me directly, please" request.
    if (qClass & 0x8000) plan->unicast = true;

    uint16_t klass = (uint16_t)(qClass & 0x7FFF);
    if (klass != MDNS_CLASS_IN && klass != MDNS_CLASS_ANY) continue;

    plan->replyMask |= replyMaskForQuestion(name, qType, names);
  }

  // The answer sections are deliberately not read - see the file header.

  // Whoever asks about the service wants the SRV, TXT and A next, so send them
  // now rather than making them ask three more times.
  if (plan->replyMask & MDNS_REPLY_PTR_SVC) {
    plan->replyMask |= MDNS_REPLY_SRV | MDNS_REPLY_TXT | MDNS_REPLY_A;
  }
  if (plan->replyMask & (MDNS_REPLY_SRV | MDNS_REPLY_TXT)) {
    plan->replyMask |= MDNS_REPLY_A;
  }
  return true;
}

// ---- building ----

static bool writeU16(uint8_t* out, size_t outSize, size_t* pos, uint16_t value) {
  if (*pos + 2 > outSize) return false;
  out[(*pos)++] = (uint8_t)(value >> 8);
  out[(*pos)++] = (uint8_t)(value & 0xFF);
  return true;
}

static bool writeU32(uint8_t* out, size_t outSize, size_t* pos, uint32_t value) {
  if (*pos + 4 > outSize) return false;
  out[(*pos)++] = (uint8_t)(value >> 24);
  out[(*pos)++] = (uint8_t)(value >> 16);
  out[(*pos)++] = (uint8_t)(value >> 8);
  out[(*pos)++] = (uint8_t)(value & 0xFF);
  return true;
}

// Encode a dotted name as the wire form: each label prefixed with its length,
// terminated by a zero byte. We never emit compression pointers - the packets
// are small enough that it is not worth the bookkeeping, and every resolver
// accepts the long form.
static bool writeName(uint8_t* out, size_t outSize, size_t* pos, const char* name) {
  const char* label = name;
  while (*label) {
    const char* dot      = strchr(label, '.');
    size_t      labelLen = dot ? (size_t)(dot - label) : strlen(label);
    if (labelLen == 0 || labelLen > 63) return false;
    if (*pos + 1 + labelLen > outSize)  return false;
    out[(*pos)++] = (uint8_t)labelLen;
    memcpy(out + *pos, label, labelLen);
    *pos  += labelLen;
    label += labelLen;
    if (*label == '.') label++;
  }
  if (*pos + 1 > outSize) return false;
  out[(*pos)++] = 0;
  return true;
}

static bool writeRecord(uint8_t* out, size_t outSize, size_t* pos,
                        const char* name, uint16_t type, uint16_t rrClass,
                        uint32_t ttl, const uint8_t* rdata, uint16_t rdLen) {
  if (!writeName(out, outSize, pos, name))   return false;
  if (!writeU16(out, outSize, pos, type))    return false;
  if (!writeU16(out, outSize, pos, rrClass)) return false;
  if (!writeU32(out, outSize, pos, ttl))     return false;
  if (!writeU16(out, outSize, pos, rdLen))   return false;
  if (*pos + rdLen > outSize)                return false;
  memcpy(out + *pos, rdata, rdLen);
  *pos += rdLen;
  return true;
}

static bool writePtrRecord(uint8_t* out, size_t outSize, size_t* pos,
                           const char* name, uint16_t rrClass, uint32_t ttl,
                           const char* target) {
  uint8_t rdata[MDNS_RDATA_MAX];
  size_t  rdLen = 0;
  if (!writeName(rdata, sizeof(rdata), &rdLen, target)) return false;
  return writeRecord(out, outSize, pos, name, MDNS_TYPE_PTR, rrClass, ttl,
                     rdata, (uint16_t)rdLen);
}

size_t mdnsBuildResponse(uint8_t* out, size_t outSize, const MdnsQueryPlan& plan,
                         const MdnsNames& names, uint32_t ipv4, uint16_t port,
                         const MdnsTxt& txt) {
  // An announcement asks for MDNS_REPLY_ALL whether or not there is a service,
  // so drop the service records here rather than at every caller.
  uint8_t mask = plan.replyMask;
  if (!names.hasService) mask &= (uint8_t)~MDNS_REPLY_SERVICE;

  if (mask == 0 || outSize < MDNS_HEADER_LEN) return 0;

  // A legacy resolver gets short TTLs and no cache-flush bit (RFC 6762 6.7).
  uint32_t hostTtl = plan.legacy ? MDNS_LEGACY_TTL : MDNS_HOST_TTL;
  uint32_t svcTtl  = plan.legacy ? MDNS_LEGACY_TTL : MDNS_SERVICE_TTL;
  // Records only we can own are marked unique so resolvers drop stale copies.
  uint16_t unique  = plan.legacy ? MDNS_CLASS_IN
                                 : (uint16_t)(MDNS_CLASS_IN | MDNS_CACHE_FLUSH);

  size_t   pos     = MDNS_HEADER_LEN;
  uint16_t answers = 0;

  // Service-type PTRs are shared - other devices publish the same service
  // type - so they never carry the cache-flush bit.
  if (mask & MDNS_REPLY_PTR_SVC) {
    if (!writePtrRecord(out, outSize, &pos, names.service, MDNS_CLASS_IN,
                        svcTtl, names.instance)) return 0;
    answers++;
  }
  if (mask & MDNS_REPLY_PTR_META) {
    if (!writePtrRecord(out, outSize, &pos, MDNS_META_QUERY, MDNS_CLASS_IN,
                        svcTtl, names.service)) return 0;
    answers++;
  }
  if (mask & MDNS_REPLY_SRV) {
    uint8_t rdata[MDNS_RDATA_MAX];
    size_t  rdLen = 0;
    if (!writeU16(rdata, sizeof(rdata), &rdLen, 0))            return 0;  // priority
    if (!writeU16(rdata, sizeof(rdata), &rdLen, 0))            return 0;  // weight
    if (!writeU16(rdata, sizeof(rdata), &rdLen, port))         return 0;
    if (!writeName(rdata, sizeof(rdata), &rdLen, names.host))  return 0;
    if (!writeRecord(out, outSize, &pos, names.instance, MDNS_TYPE_SRV,
                     unique, hostTtl, rdata, (uint16_t)rdLen)) return 0;
    answers++;
  }
  if (mask & MDNS_REPLY_TXT) {
    // DNS-SD wants a TXT even when there is nothing to say; the empty form is
    // a single zero byte (RFC 6763 6.1), not a zero-length record.
    const uint8_t empty[1] = { 0 };
    const uint8_t* rdata   = (txt.len > 0) ? txt.data : empty;
    uint16_t       rdLen   = (txt.len > 0) ? txt.len  : (uint16_t)sizeof(empty);
    if (!writeRecord(out, outSize, &pos, names.instance, MDNS_TYPE_TXT,
                     unique, hostTtl, rdata, rdLen)) return 0;
    answers++;
  }
  if (mask & MDNS_REPLY_A) {
    const uint8_t rdata[4] = {
      (uint8_t)(ipv4 >> 24), (uint8_t)(ipv4 >> 16),
      (uint8_t)(ipv4 >> 8),  (uint8_t)(ipv4 & 0xFF)
    };
    if (!writeRecord(out, outSize, &pos, names.host, MDNS_TYPE_A,
                     unique, hostTtl, rdata, sizeof(rdata))) return 0;
    answers++;
  }
  if (mask & MDNS_REPLY_PTR_REV) {
    if (!writePtrRecord(out, outSize, &pos, names.reverse, unique,
                        hostTtl, names.host)) return 0;
    answers++;
  }

  if (answers == 0) return 0;

  // Header last, now that the answer count is known. Everything goes in the
  // answer section: mDNS allows extra answers, and one section keeps the
  // builder to a single pass.
  size_t hdr = 0;
  writeU16(out, outSize, &hdr, plan.legacy ? plan.queryId : 0);
  writeU16(out, outSize, &hdr, 0x8400);   // response, authoritative
  writeU16(out, outSize, &hdr, 0);        // no questions echoed back
  writeU16(out, outSize, &hdr, answers);
  writeU16(out, outSize, &hdr, 0);        // no authority records
  writeU16(out, outSize, &hdr, 0);        // no additional records

  return pos;
}
