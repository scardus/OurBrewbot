// Native (host) tests for the MicroMDNS packet layer in
// lib/MicroMDNS/src/MdnsPacket.cpp:
// mdnsBuildNames(), mdnsTxtAdd(), mdnsDecodeName(), mdnsParseQuery() and
// mdnsBuildResponse().
//
// MdnsPacket.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test, and so the file-static helpers
// (replyMaskForQuestion, writeName, ...) are reachable from here. Unlike the
// other native suites this one needs no stubs at all - the module deliberately
// touches nothing but plain byte buffers.
//
// Several cases guard the decisions that make this module safe on a busy
// network, where its predecessor caused 50 hardware-watchdog resets between
// May and July 2026 (see project memory):
//   - a query's answer sections are never read, however many records the
//     header claims, because reading them is what made the old library
//     allocate ~544 bytes per record inside the WiFi receive callback
//   - compression pointers must point strictly backwards, so a decode can
//     never loop or run off the end of a truncated read
//   - every write is bounds checked against a caller-supplied buffer

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "../../lib/MicroMDNS/src/MdnsPacket.cpp"

// ---- fixture ----

static MdnsNames g_names;
static MdnsTxt   g_txt;

// 192.168.0.207, in the host order this module uses (first octet = MSB).
#define TEST_IP  0xC0A800CFu

void setUp(void) {
  mdnsBuildNames("ourbrewbot-2924fa", "http", "tcp", TEST_IP, &g_names);
  memset(&g_txt, 0, sizeof(g_txt));
}

void tearDown(void) {}

// ---- helpers for composing query packets ----

// Wire-encode a dotted name at pos, returning the new position.
static size_t putName(uint8_t* p, size_t pos, const char* name) {
  const char* label = name;
  while (*label) {
    const char* dot      = strchr(label, '.');
    size_t      labelLen = dot ? (size_t)(dot - label) : strlen(label);
    p[pos++] = (uint8_t)labelLen;
    memcpy(p + pos, label, labelLen);
    pos   += labelLen;
    label += labelLen;
    if (*label == '.') label++;
  }
  p[pos++] = 0;
  return pos;
}

static size_t putU16(uint8_t* p, size_t pos, uint16_t v) {
  p[pos++] = (uint8_t)(v >> 8);
  p[pos++] = (uint8_t)(v & 0xFF);
  return pos;
}

// A one-question query. anCount lets a test claim answers that aren't there.
static size_t buildQuery(uint8_t* p, const char* qname, uint16_t qtype,
                         uint16_t qclass, uint16_t id = 0, uint16_t flags = 0,
                         uint16_t anCount = 0) {
  size_t pos = 0;
  pos = putU16(p, pos, id);
  pos = putU16(p, pos, flags);
  pos = putU16(p, pos, 1);         // QDCOUNT
  pos = putU16(p, pos, anCount);   // ANCOUNT
  pos = putU16(p, pos, 0);         // NSCOUNT
  pos = putU16(p, pos, 0);         // ARCOUNT
  pos = putName(p, pos, qname);
  pos = putU16(p, pos, qtype);
  pos = putU16(p, pos, qclass);
  return pos;
}

// ---- helpers for inspecting a built response ----

struct Rec {
  char           name[MDNS_MAX_NAME];
  uint16_t       type;
  uint16_t       rrClass;
  uint32_t       ttl;
  const uint8_t* rdata;
  uint16_t       rdLen;
};

static uint16_t getU16(const uint8_t* p, size_t pos) {
  return (uint16_t)((p[pos] << 8) | p[pos + 1]);
}

static uint32_t getU32(const uint8_t* p, size_t pos) {
  return ((uint32_t)p[pos] << 24) | ((uint32_t)p[pos + 1] << 16) |
         ((uint32_t)p[pos + 2] << 8) | (uint32_t)p[pos + 3];
}

// Walk the answer section, filling recs[]. Returns the number of records read,
// or -1 if the packet doesn't hold together.
static int walkAnswers(const uint8_t* pkt, size_t len, Rec* recs, int maxRecs) {
  int    want = (int)getU16(pkt, 6);
  size_t pos  = 12;

  // Step over any questions - only a legacy answer repeats one.
  for (int q = 0; q < (int)getU16(pkt, 4); q++) {
    char   name[MDNS_MAX_NAME];
    size_t next = 0;
    if (!mdnsDecodeName(pkt, len, pos, name, sizeof(name), &next)) return -1;
    pos = next + 4;
  }

  if (want > maxRecs) return -1;
  for (int i = 0; i < want; i++) {
    size_t next = 0;
    if (!mdnsDecodeName(pkt, len, pos, recs[i].name, sizeof(recs[i].name), &next)) return -1;
    if (next + 10 > len) return -1;
    recs[i].type    = getU16(pkt, next);
    recs[i].rrClass = getU16(pkt, next + 2);
    recs[i].ttl     = getU32(pkt, next + 4);
    recs[i].rdLen   = getU16(pkt, next + 8);
    recs[i].rdata   = pkt + next + 10;
    pos = next + 10 + recs[i].rdLen;
    if (pos > len) return -1;
  }
  return want;
}

static const Rec* findRec(const Rec* recs, int count, uint16_t type) {
  for (int i = 0; i < count; i++) {
    if (recs[i].type == type) return &recs[i];
  }
  return nullptr;
}

// Build a response for a mask, with the multicast defaults.
static size_t buildFor(uint8_t* out, size_t outSize, uint8_t mask,
                       bool legacy = false, uint16_t queryId = 0) {
  MdnsQueryPlan plan;
  plan.replyMask = mask;
  plan.unicast   = legacy;
  plan.legacy    = legacy;
  plan.queryId   = queryId;
  return mdnsBuildResponse(out, outSize, plan, g_names, TEST_IP, 80, g_txt);
}

// ================================================================
// A. name construction
// ================================================================

void test_names_are_built_from_the_hostname(void) {
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa.local", g_names.host);
  TEST_ASSERT_EQUAL_STRING("_http._tcp.local", g_names.service);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa._http._tcp.local", g_names.instance);
}

// in-addr.arpa reverses the octets; getting this backwards is the classic way
// to silently never answer a reverse lookup.
void test_reverse_name_reverses_the_octets(void) {
  TEST_ASSERT_EQUAL_STRING("207.0.168.192.in-addr.arpa", g_names.reverse);
}

void test_names_are_lowercased(void) {
  MdnsNames n;
  mdnsBuildNames("OurBrewBot-2924FA", "http", "tcp", TEST_IP, &n);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa.local", n.host);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa._http._tcp.local", n.instance);
}

// ================================================================
// B. name decoding
// ================================================================

void test_decode_reads_a_plain_name(void) {
  uint8_t pkt[64];
  size_t  end = putName(pkt, 0, "ourbrewbot-2924fa.local");
  char    out[MDNS_MAX_NAME];
  size_t  next = 0;

  TEST_ASSERT_TRUE(mdnsDecodeName(pkt, end, 0, out, sizeof(out), &next));
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa.local", out);
  TEST_ASSERT_EQUAL_UINT32(end, next);
}

void test_decode_lowercases_as_it_reads(void) {
  uint8_t pkt[64];
  size_t  end = putName(pkt, 0, "OurBrewBot.LOCAL");
  char    out[MDNS_MAX_NAME];
  size_t  next = 0;

  TEST_ASSERT_TRUE(mdnsDecodeName(pkt, end, 0, out, sizeof(out), &next));
  TEST_ASSERT_EQUAL_STRING("ourbrewbot.local", out);
}

void test_decode_follows_a_backward_compression_pointer(void) {
  uint8_t pkt[64];
  size_t  pos = putName(pkt, 0, "local");     // target at offset 0
  size_t  namePos = pos;
  pkt[pos++] = 3;                             // "www"
  memcpy(pkt + pos, "www", 3);
  pos += 3;
  pkt[pos++] = 0xC0;                          // pointer to offset 0
  pkt[pos++] = 0x00;

  char   out[MDNS_MAX_NAME];
  size_t next = 0;
  TEST_ASSERT_TRUE(mdnsDecodeName(pkt, pos, namePos, out, sizeof(out), &next));
  TEST_ASSERT_EQUAL_STRING("www.local", out);
  // The name ends after the pointer, not after the target it jumped to.
  TEST_ASSERT_EQUAL_UINT32(pos, next);
}

// A pointer that goes forwards (or to itself) is how a malformed packet turns
// a decoder into an infinite loop. Both must be refused.
void test_decode_rejects_a_forward_pointer(void) {
  uint8_t pkt[16] = { 0xC0, 0x08, 0, 0, 0, 0, 0, 0, 1, 'a', 0 };
  char    out[MDNS_MAX_NAME];
  size_t  next = 0;
  TEST_ASSERT_FALSE(mdnsDecodeName(pkt, sizeof(pkt), 0, out, sizeof(out), &next));
}

void test_decode_rejects_a_self_pointer(void) {
  uint8_t pkt[4] = { 0xC0, 0x00, 0, 0 };
  char    out[MDNS_MAX_NAME];
  size_t  next = 0;
  TEST_ASSERT_FALSE(mdnsDecodeName(pkt, sizeof(pkt), 0, out, sizeof(out), &next));
}

void test_decode_rejects_a_label_running_past_the_buffer(void) {
  uint8_t pkt[4] = { 40, 'a', 'b', 'c' };   // claims 40 bytes, has 3
  char    out[MDNS_MAX_NAME];
  size_t  next = 0;
  TEST_ASSERT_FALSE(mdnsDecodeName(pkt, sizeof(pkt), 0, out, sizeof(out), &next));
}

void test_decode_rejects_an_unterminated_name(void) {
  uint8_t pkt[8];
  pkt[0] = 3;
  memcpy(pkt + 1, "abc", 3);   // no terminating zero inside len
  char   out[MDNS_MAX_NAME];
  size_t next = 0;
  TEST_ASSERT_FALSE(mdnsDecodeName(pkt, 4, 0, out, sizeof(out), &next));
}

void test_decode_refuses_to_overflow_the_output_buffer(void) {
  uint8_t pkt[64];
  size_t  end = putName(pkt, 0, "ourbrewbot-2924fa.local");
  char    small[8];
  size_t  next = 0;
  TEST_ASSERT_FALSE(mdnsDecodeName(pkt, end, 0, small, sizeof(small), &next));
}

// ================================================================
// C. query parsing
// ================================================================

void test_query_for_our_host_asks_for_the_a_record(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_A, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A | MDNS_REPLY_NSEC, plan.replyMask);
  TEST_ASSERT_FALSE(plan.unicast);
  TEST_ASSERT_FALSE(plan.legacy);
}

void test_query_is_matched_case_insensitively(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "OurBrewbot-2924FA.Local", MDNS_TYPE_A, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A | MDNS_REPLY_NSEC, plan.replyMask);
}

void test_query_for_another_host_asks_for_nothing(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "somebody-else.local", MDNS_TYPE_A, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(0, plan.replyMask);
}

// A browser asking for the service type gets the whole set, so it never has to
// come back for the SRV, TXT and A separately.
void test_service_query_pulls_in_srv_txt_and_a(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "_http._tcp.local", MDNS_TYPE_PTR, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_PTR_SVC | MDNS_REPLY_SRV |
                          MDNS_REPLY_TXT | MDNS_REPLY_A | MDNS_REPLY_NSEC, plan.replyMask);
}

void test_srv_query_pulls_in_the_a_record(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa._http._tcp.local", MDNS_TYPE_SRV, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_SRV | MDNS_REPLY_A | MDNS_REPLY_NSEC, plan.replyMask);
}

void test_txt_query_pulls_in_the_a_record(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa._http._tcp.local", MDNS_TYPE_TXT, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_TXT | MDNS_REPLY_A | MDNS_REPLY_NSEC, plan.replyMask);
}

void test_meta_query_enumerates_our_service_type(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "_services._dns-sd._udp.local", MDNS_TYPE_PTR, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_PTR_META, plan.replyMask);
}

void test_reverse_query_asks_for_the_reverse_ptr(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "207.0.168.192.in-addr.arpa", MDNS_TYPE_PTR, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_PTR_REV, plan.replyMask);
}

void test_any_query_for_the_host_asks_for_the_a_record(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_ANY, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A | MDNS_REPLY_NSEC, plan.replyMask);
}

// Asking our host name for a type we don't publish gets an NSEC saying the A
// record is all there is (RFC 6762 section 6.1), not silence.
void test_wrong_type_for_our_host_gets_an_nsec(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_SRV, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_NSEC, plan.replyMask);
}

void test_query_in_a_foreign_class_is_ignored(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_A, 3);  // CHAOS

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(0, plan.replyMask);
}

void test_unicast_bit_requests_a_direct_answer(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_A, 0x8001);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A | MDNS_REPLY_NSEC, plan.replyMask);
  TEST_ASSERT_TRUE(plan.unicast);
  TEST_ASSERT_FALSE(plan.legacy);
}

// A resolver that isn't on 5353 is an ordinary DNS client; it needs its own
// query ID back.
void test_query_from_a_legacy_port_is_answered_directly(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_A, 1, 0xBEEF);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 49152, &plan));
  TEST_ASSERT_TRUE(plan.legacy);
  TEST_ASSERT_TRUE(plan.unicast);
  TEST_ASSERT_EQUAL_UINT16(0xBEEF, plan.queryId);
}

void test_a_response_is_not_treated_as_a_query(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_A, 1, 0, 0x8400);

  MdnsQueryPlan plan;
  TEST_ASSERT_FALSE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(0, plan.replyMask);
}

void test_a_non_standard_opcode_is_refused(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_A, 1, 0, 0x2800);

  MdnsQueryPlan plan;
  TEST_ASSERT_FALSE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
}

void test_a_query_with_no_questions_is_refused(void) {
  uint8_t pkt[12] = { 0 };
  MdnsQueryPlan plan;
  TEST_ASSERT_FALSE(mdnsParseQuery(pkt, sizeof(pkt), g_names, 5353, &plan));
}

void test_a_runt_packet_is_refused(void) {
  uint8_t pkt[8] = { 0 };
  MdnsQueryPlan plan;
  TEST_ASSERT_FALSE(mdnsParseQuery(pkt, sizeof(pkt), g_names, 5353, &plan));
}

// The regression that matters most. The old library read every record in the
// answer sections to do known-answer suppression, allocating as it went. We
// claim 500 answers and supply none: parsing must still succeed, still produce
// the right mask, and never look past the questions.
void test_answer_sections_are_never_read(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_A, 1, 0, 0, 500);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A | MDNS_REPLY_NSEC, plan.replyMask);
}

// Two questions, the second truncated: the first must still be answered and
// the parser must stop rather than read rubbish.
void test_a_truncated_second_question_does_not_lose_the_first(void) {
  uint8_t pkt[128];
  size_t  pos = 0;
  pos = putU16(pkt, pos, 0);
  pos = putU16(pkt, pos, 0);
  pos = putU16(pkt, pos, 2);      // QDCOUNT says two
  pos = putU16(pkt, pos, 0);
  pos = putU16(pkt, pos, 0);
  pos = putU16(pkt, pos, 0);
  pos = putName(pkt, pos, "ourbrewbot-2924fa.local");
  pos = putU16(pkt, pos, MDNS_TYPE_A);
  pos = putU16(pkt, pos, 1);
  pkt[pos++] = 5;                 // a label that never completes
  memcpy(pkt + pos, "abc", 3);
  pos += 3;

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, pos, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A | MDNS_REPLY_NSEC, plan.replyMask);
}

// ================================================================
// D. response building
// ================================================================

void test_nothing_is_built_for_an_empty_mask(void) {
  uint8_t out[512];
  TEST_ASSERT_EQUAL_UINT32(0, buildFor(out, sizeof(out), 0));
}

void test_nothing_is_built_when_it_would_not_fit(void) {
  uint8_t out[32];
  TEST_ASSERT_EQUAL_UINT32(0, buildFor(out, sizeof(out), MDNS_REPLY_ALL));
}

void test_a_record_carries_our_address_in_network_order(void) {
  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_A);
  TEST_ASSERT_TRUE(len > 12);

  Rec recs[8];
  int count = walkAnswers(out, len, recs, 8);
  TEST_ASSERT_EQUAL_INT(1, count);

  const Rec* a = findRec(recs, count, MDNS_TYPE_A);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa.local", a->name);
  TEST_ASSERT_EQUAL_UINT16(4, a->rdLen);
  TEST_ASSERT_EQUAL_UINT8(192, a->rdata[0]);
  TEST_ASSERT_EQUAL_UINT8(168, a->rdata[1]);
  TEST_ASSERT_EQUAL_UINT8(0,   a->rdata[2]);
  TEST_ASSERT_EQUAL_UINT8(207, a->rdata[3]);
}

void test_multicast_response_has_a_zero_id_and_the_authoritative_flag(void) {
  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_A);
  TEST_ASSERT_TRUE(len > 12);
  TEST_ASSERT_EQUAL_UINT16(0, getU16(out, 0));
  TEST_ASSERT_EQUAL_UINT16(0x8400, getU16(out, 2));
  TEST_ASSERT_EQUAL_UINT16(0, getU16(out, 4));   // no questions echoed
  TEST_ASSERT_EQUAL_UINT16(0, getU16(out, 8));   // no authority records
  TEST_ASSERT_EQUAL_UINT16(0, getU16(out, 10));  // no additional records
}

void test_unique_records_set_the_cache_flush_bit(void) {
  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_A);
  Rec     recs[8];
  int     count = walkAnswers(out, len, recs, 8);

  const Rec* a = findRec(recs, count, MDNS_TYPE_A);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_EQUAL_UINT16(0x8001, a->rrClass);
  TEST_ASSERT_EQUAL_UINT32(120, a->ttl);
}

// The service-type PTR is shared with every other _http._tcp device on the
// network, so marking it unique would tell resolvers to discard theirs.
void test_the_service_ptr_is_shared_not_unique(void) {
  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_PTR_SVC);
  Rec     recs[8];
  int     count = walkAnswers(out, len, recs, 8);

  const Rec* ptr = findRec(recs, count, MDNS_TYPE_PTR);
  TEST_ASSERT_NOT_NULL(ptr);
  TEST_ASSERT_EQUAL_STRING("_http._tcp.local", ptr->name);
  TEST_ASSERT_EQUAL_UINT16(0x0001, ptr->rrClass);
  TEST_ASSERT_EQUAL_UINT32(4500, ptr->ttl);
}

void test_the_service_ptr_points_at_our_instance(void) {
  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_PTR_SVC);
  Rec     recs[8];
  int     count = walkAnswers(out, len, recs, 8);

  const Rec* ptr = findRec(recs, count, MDNS_TYPE_PTR);
  TEST_ASSERT_NOT_NULL(ptr);

  char   target[MDNS_MAX_NAME];
  size_t next = 0;
  TEST_ASSERT_TRUE(mdnsDecodeName(out, len, (size_t)(ptr->rdata - out),
                                  target, sizeof(target), &next));
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa._http._tcp.local", target);
}

void test_srv_carries_the_port_and_the_host_as_target(void) {
  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_SRV);
  Rec     recs[8];
  int     count = walkAnswers(out, len, recs, 8);

  const Rec* srv = findRec(recs, count, MDNS_TYPE_SRV);
  TEST_ASSERT_NOT_NULL(srv);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa._http._tcp.local", srv->name);
  TEST_ASSERT_EQUAL_UINT16(0, getU16(srv->rdata, 0));    // priority
  TEST_ASSERT_EQUAL_UINT16(0, getU16(srv->rdata, 2));    // weight
  TEST_ASSERT_EQUAL_UINT16(80, getU16(srv->rdata, 4));   // port

  char   target[MDNS_MAX_NAME];
  size_t next = 0;
  TEST_ASSERT_TRUE(mdnsDecodeName(out, len, (size_t)(srv->rdata - out) + 6,
                                  target, sizeof(target), &next));
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa.local", target);
}

// An empty DNS-SD TXT is one zero byte, not a zero-length record - some
// browsers ignore the service entirely if this is wrong.
void test_empty_txt_is_a_single_zero_byte(void) {
  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_TXT);
  Rec     recs[8];
  int     count = walkAnswers(out, len, recs, 8);

  const Rec* txt = findRec(recs, count, MDNS_TYPE_TXT);
  TEST_ASSERT_NOT_NULL(txt);
  TEST_ASSERT_EQUAL_UINT16(1, txt->rdLen);
  TEST_ASSERT_EQUAL_UINT8(0, txt->rdata[0]);
}

void test_an_announcement_carries_every_published_record(void) {
  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_ALL);
  Rec     recs[16];
  int     count = walkAnswers(out, len, recs, 16);

  TEST_ASSERT_EQUAL_INT(6, count);
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_NSEC));
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_A));
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_SRV));
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_TXT));
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_PTR));
}

void test_the_answer_count_matches_the_records_written(void) {
  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_ALL);
  Rec     recs[16];

  // walkAnswers returns -1 if the header count and the bytes disagree.
  TEST_ASSERT_EQUAL_INT((int)getU16(out, 6), walkAnswers(out, len, recs, 16));
}

// A legacy resolver can't interpret a cache-flush bit and won't see a goodbye,
// so it gets a plain class and a short TTL, plus its own query ID.
void test_legacy_response_echoes_the_id_and_drops_the_cache_flush_bit(void) {
  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_A, true, 0xBEEF);
  TEST_ASSERT_TRUE(len > 12);
  TEST_ASSERT_EQUAL_UINT16(0xBEEF, getU16(out, 0));

  Rec recs[8];
  int count = walkAnswers(out, len, recs, 8);
  const Rec* a = findRec(recs, count, MDNS_TYPE_A);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_EQUAL_UINT16(0x0001, a->rrClass);
  TEST_ASSERT_EQUAL_UINT32(10, a->ttl);
}

// End to end: the query a browser actually sends, through to a packet that
// holds together when walked back.
void test_a_service_query_round_trips_into_a_complete_answer(void) {
  uint8_t pkt[128];
  size_t  qlen = buildQuery(pkt, "_http._tcp.local", MDNS_TYPE_PTR, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, qlen, g_names, 5353, &plan));

  uint8_t out[512];
  size_t  len = mdnsBuildResponse(out, sizeof(out), plan, g_names, TEST_IP, 80,
                                    g_txt);
  TEST_ASSERT_TRUE(len > 12);

  Rec recs[16];
  int count = walkAnswers(out, len, recs, 16);
  TEST_ASSERT_EQUAL_INT(5, count);
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_PTR));
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_SRV));
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_TXT));
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_A));
}

// ================================================================
// E. library configuration: host only, custom service, TXT, sizes
// ================================================================

// Build a response from any names/port/TXT, not just the fixture's.
static size_t buildWith(uint8_t* out, size_t outSize, uint8_t mask,
                        const MdnsNames& names, uint16_t port, const MdnsTxt& txt) {
  MdnsQueryPlan plan;
  plan.replyMask = mask;
  plan.unicast   = false;
  plan.legacy    = false;
  plan.queryId   = 0;
  return mdnsBuildResponse(out, outSize, plan, names, TEST_IP, port, txt);
}

void test_host_only_names_have_no_service(void) {
  MdnsNames n;
  mdnsBuildNames("ourbrewbot-2924fa", NULL, NULL, TEST_IP, &n);
  TEST_ASSERT_FALSE(n.hasService);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa.local", n.host);
  TEST_ASSERT_EQUAL_STRING("", n.service);
  TEST_ASSERT_EQUAL_STRING("", n.instance);
}

void test_host_only_ignores_service_and_meta_queries(void) {
  MdnsNames n;
  mdnsBuildNames("ourbrewbot-2924fa", NULL, NULL, TEST_IP, &n);

  uint8_t       pkt[128];
  MdnsQueryPlan plan;
  size_t        qlen = buildQuery(pkt, "_http._tcp.local", MDNS_TYPE_PTR, 1);
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, qlen, n, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(0, plan.replyMask);

  qlen = buildQuery(pkt, "_services._dns-sd._udp.local", MDNS_TYPE_PTR, 1);
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, qlen, n, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(0, plan.replyMask);

  qlen = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_A, 1);
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, qlen, n, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A | MDNS_REPLY_NSEC, plan.replyMask);
}

// With no service the instance name is "", and so is a query for the root
// name. Without the hasService check that query would match and we would
// answer with SRV/TXT records for a service that does not exist.
void test_host_only_root_query_asks_for_nothing(void) {
  MdnsNames n;
  mdnsBuildNames("ourbrewbot-2924fa", NULL, NULL, TEST_IP, &n);

  uint8_t       pkt[64];
  MdnsQueryPlan plan;
  size_t        qlen = buildQuery(pkt, "", MDNS_TYPE_ANY, 1);
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, qlen, n, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(0, plan.replyMask);
}

void test_host_only_announcement_carries_only_the_address_records(void) {
  MdnsNames n;
  mdnsBuildNames("ourbrewbot-2924fa", NULL, NULL, TEST_IP, &n);

  uint8_t out[512];
  size_t  len = buildWith(out, sizeof(out), MDNS_REPLY_ALL, n, 0, g_txt);
  Rec     recs[8];
  int     count = walkAnswers(out, len, recs, 8);
  TEST_ASSERT_EQUAL_INT(2, count);
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_A));
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_NSEC));
}

void test_a_custom_service_type_is_answered_instead_of_http(void) {
  MdnsNames n;
  mdnsBuildNames("printer-01", "ipp", "tcp", TEST_IP, &n);
  TEST_ASSERT_EQUAL_STRING("_ipp._tcp.local", n.service);
  TEST_ASSERT_EQUAL_STRING("printer-01._ipp._tcp.local", n.instance);

  uint8_t       pkt[128];
  MdnsQueryPlan plan;
  size_t        qlen = buildQuery(pkt, "_ipp._tcp.local", MDNS_TYPE_PTR, 1);
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, qlen, n, 5353, &plan));
  TEST_ASSERT_TRUE(plan.replyMask & MDNS_REPLY_PTR_SVC);

  qlen = buildQuery(pkt, "_http._tcp.local", MDNS_TYPE_PTR, 1);
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, qlen, n, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(0, plan.replyMask);
}

void test_service_names_are_lowercased(void) {
  MdnsNames n;
  mdnsBuildNames("ourbrewbot-2924fa", "HTTP", "tcp", TEST_IP, &n);
  TEST_ASSERT_EQUAL_STRING("_http._tcp.local", n.service);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa._http._tcp.local", n.instance);
}

void test_srv_carries_a_custom_port(void) {
  uint8_t out[512];
  size_t  len = buildWith(out, sizeof(out), MDNS_REPLY_SRV, g_names, 8080, g_txt);
  Rec     recs[8];
  int     count = walkAnswers(out, len, recs, 8);

  const Rec* srv = findRec(recs, count, MDNS_TYPE_SRV);
  TEST_ASSERT_NOT_NULL(srv);
  TEST_ASSERT_EQUAL_UINT16(8080, getU16(srv->rdata, 4));
}

void test_txt_entries_are_sent_length_prefixed(void) {
  TEST_ASSERT_TRUE(mdnsTxtAdd(&g_txt, "path=/"));
  TEST_ASSERT_TRUE(mdnsTxtAdd(&g_txt, "v=1"));

  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_TXT);
  Rec     recs[8];
  int     count = walkAnswers(out, len, recs, 8);

  const Rec* txt = findRec(recs, count, MDNS_TYPE_TXT);
  TEST_ASSERT_NOT_NULL(txt);
  const uint8_t expected[] = { 6, 'p', 'a', 't', 'h', '=', '/', 3, 'v', '=', '1' };
  TEST_ASSERT_EQUAL_UINT16(sizeof(expected), txt->rdLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, txt->rdata, sizeof(expected));
}

void test_txt_add_rejects_an_empty_entry(void) {
  TEST_ASSERT_FALSE(mdnsTxtAdd(&g_txt, ""));
  TEST_ASSERT_EQUAL_UINT8(0, g_txt.len);
}

// A 63-character entry takes all 64 bytes; the next one must be refused and
// must not leave half an entry behind.
void test_txt_add_refuses_an_entry_that_does_not_fit(void) {
  char entry[64];
  memset(entry, 'a', 63);
  entry[63] = '\0';
  TEST_ASSERT_TRUE(mdnsTxtAdd(&g_txt, entry));
  TEST_ASSERT_EQUAL_UINT8(MDNS_MAX_TXT_LEN, g_txt.len);

  TEST_ASSERT_FALSE(mdnsTxtAdd(&g_txt, "b"));
  TEST_ASSERT_EQUAL_UINT8(MDNS_MAX_TXT_LEN, g_txt.len);
}

void test_txt_add_refuses_an_entry_longer_than_the_whole_buffer(void) {
  char entry[65];
  memset(entry, 'a', 64);
  entry[64] = '\0';
  TEST_ASSERT_FALSE(mdnsTxtAdd(&g_txt, entry));
  TEST_ASSERT_EQUAL_UINT8(0, g_txt.len);
}

// The limits in MdnsPacket.h are chosen so the biggest possible response still
// fits the responder's transmit buffer. If they are ever raised without
// raising MDNS_TX_SIZE, the build returns 0 and the device silently stops
// answering - this catches that on the host instead.
//
// The biggest response is not the announcement: one query can ask several
// questions, so it can pull in the reverse PTR as well. The longest reverse
// name comes from 255.255.255.255.
void test_the_largest_response_fits_the_transmit_buffer(void) {
  char host[MDNS_MAX_HOST_LEN + 1];
  char service[MDNS_MAX_SERVICE_LEN + 1];
  memset(host, 'h', MDNS_MAX_HOST_LEN);
  host[MDNS_MAX_HOST_LEN] = '\0';
  memset(service, 's', MDNS_MAX_SERVICE_LEN);
  service[MDNS_MAX_SERVICE_LEN] = '\0';

  const uint32_t longestIp = 0xFFFFFFFFu;   // 255.255.255.255
  MdnsNames n;
  mdnsBuildNames(host, service, "tcp", longestIp, &n);

  char entry[64];
  memset(entry, 't', 63);
  entry[63] = '\0';
  TEST_ASSERT_TRUE(mdnsTxtAdd(&g_txt, entry));

  MdnsQueryPlan plan;
  plan.replyMask = MDNS_REPLY_ALL | MDNS_REPLY_PTR_REV;
  plan.unicast   = false;
  plan.legacy    = false;
  plan.queryId   = 0;

  uint8_t out[MDNS_TX_SIZE];
  size_t  len = mdnsBuildResponse(out, sizeof(out), plan, n, longestIp, 65535, g_txt);
  TEST_ASSERT_TRUE(len > 0);

  Rec recs[8];
  TEST_ASSERT_EQUAL_INT(7, walkAnswers(out, len, recs, 8));
}

// ================================================================
// F. standards compliance: NSEC, legacy echo, rate limit, name rules
// ================================================================

// RFC 6762 section 6.1's restricted NSEC: our own name as the "next" name,
// then a one-byte type bitmap for window 0 with only type 1 (A) set.
void test_nsec_says_only_an_a_record_exists(void) {
  uint8_t out[512];
  size_t  len = buildFor(out, sizeof(out), MDNS_REPLY_NSEC);
  Rec     recs[8];
  int     count = walkAnswers(out, len, recs, 8);
  TEST_ASSERT_EQUAL_INT(1, count);

  const Rec* nsec = findRec(recs, count, MDNS_TYPE_NSEC);
  TEST_ASSERT_NOT_NULL(nsec);
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa.local", nsec->name);
  TEST_ASSERT_EQUAL_UINT16(0x8001, nsec->rrClass);
  TEST_ASSERT_EQUAL_UINT32(120, nsec->ttl);

  char   next[MDNS_MAX_NAME];
  size_t after = 0;
  size_t start = (size_t)(nsec->rdata - out);
  TEST_ASSERT_TRUE(mdnsDecodeName(out, len, start, next, sizeof(next), &after));
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa.local", next);

  const uint8_t bitmap[] = { 0x00, 0x01, 0x40 };
  TEST_ASSERT_EQUAL_UINT32(start + nsec->rdLen, after + sizeof(bitmap));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(bitmap, out + after, sizeof(bitmap));
}

// The case NSEC exists for: a client asking for our IPv6 address. It gets a
// definite "no" instead of waiting for a timeout.
void test_aaaa_query_for_our_host_gets_only_an_nsec(void) {
  uint8_t pkt[128];
  size_t  qlen = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_AAAA, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, qlen, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_NSEC, plan.replyMask);

  uint8_t out[512];
  size_t  len = mdnsBuildResponse(out, sizeof(out), plan, g_names, TEST_IP, 80, g_txt);
  Rec     recs[8];
  int     count = walkAnswers(out, len, recs, 8);
  TEST_ASSERT_EQUAL_INT(1, count);
  TEST_ASSERT_EQUAL_UINT16(MDNS_TYPE_NSEC, recs[0].type);
}

// RFC 6762 section 6: an ordinary mDNS response MUST NOT contain questions.
void test_a_multicast_response_never_carries_a_question(void) {
  uint8_t pkt[128];
  size_t  qlen = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_A, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, qlen, g_names, 5353, &plan));

  uint8_t out[512];
  size_t  len = mdnsBuildResponse(out, sizeof(out), plan, g_names, TEST_IP, 80, g_txt);
  TEST_ASSERT_TRUE(len > 12);
  TEST_ASSERT_EQUAL_UINT16(0, getU16(out, 4));
}

// RFC 6762 section 6.7: a legacy answer MUST repeat the question. Ordinary DNS
// resolvers match answers to questions and can drop one that leaves it out.
void test_legacy_response_repeats_the_question(void) {
  uint8_t pkt[128];
  size_t  qlen = buildQuery(pkt, "OurBrewbot-2924FA.local", MDNS_TYPE_A, 1, 0xBEEF);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, qlen, g_names, 49152, &plan));
  TEST_ASSERT_TRUE(plan.legacy);

  uint8_t out[512];
  size_t  len = mdnsBuildResponse(out, sizeof(out), plan, g_names, TEST_IP, 80, g_txt);
  TEST_ASSERT_TRUE(len > 12);
  TEST_ASSERT_EQUAL_UINT16(0xBEEF, getU16(out, 0));
  TEST_ASSERT_EQUAL_UINT16(1, getU16(out, 4));   // one question

  char   name[MDNS_MAX_NAME];
  size_t next = 0;
  TEST_ASSERT_TRUE(mdnsDecodeName(out, len, 12, name, sizeof(name), &next));
  TEST_ASSERT_EQUAL_STRING("ourbrewbot-2924fa.local", name);
  TEST_ASSERT_EQUAL_UINT16(MDNS_TYPE_A, getU16(out, next));
  TEST_ASSERT_EQUAL_UINT16(1, getU16(out, next + 2));

  // The answer follows the question, and a legacy resolver gets no NSEC.
  Rec recs[8];
  int count = walkAnswers(out, len, recs, 8);
  TEST_ASSERT_EQUAL_INT(1, count);
  TEST_ASSERT_EQUAL_UINT16(MDNS_TYPE_A, recs[0].type);
}

// Two questions, the first for somebody else: the one repeated back is the one
// we actually answer.
void test_legacy_echo_is_the_first_question_we_can_answer(void) {
  uint8_t pkt[128];
  size_t  pos = 0;
  pos = putU16(pkt, pos, 0x1234);
  pos = putU16(pkt, pos, 0);
  pos = putU16(pkt, pos, 2);      // two questions
  pos = putU16(pkt, pos, 0);
  pos = putU16(pkt, pos, 0);
  pos = putU16(pkt, pos, 0);
  pos = putName(pkt, pos, "somebody-else.local");
  pos = putU16(pkt, pos, MDNS_TYPE_A);
  pos = putU16(pkt, pos, 1);
  pos = putName(pkt, pos, "ourbrewbot-2924fa.local");
  pos = putU16(pkt, pos, MDNS_TYPE_A);
  pos = putU16(pkt, pos, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, pos, g_names, 49152, &plan));
  TEST_ASSERT_EQUAL_PTR(g_names.host, plan.echoName);
  TEST_ASSERT_EQUAL_UINT16(MDNS_TYPE_A, plan.echoType);
  TEST_ASSERT_EQUAL_UINT16(1, plan.echoClass);
}

// With NSEC withheld from legacy resolvers, an AAAA question has no answer we
// could give, so nothing is sent - exactly as before NSEC was added.
void test_legacy_aaaa_query_gets_no_answer(void) {
  uint8_t pkt[128];
  size_t  qlen = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_AAAA, 1, 0xBEEF);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, qlen, g_names, 49152, &plan));

  uint8_t out[512];
  TEST_ASSERT_EQUAL_UINT32(0, mdnsBuildResponse(out, sizeof(out), plan, g_names,
                                                TEST_IP, 80, g_txt));
}

// millis() is 0 straight after a reboot. A last-sent time of 0 must not be
// mistaken for "sent just now", or the first announcement would be held back.
void test_rate_limit_allows_a_record_never_sent_even_at_time_zero(void) {
  MdnsRateLimit limit;
  memset(&limit, 0, sizeof(limit));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_ALL, mdnsRateLimitFilter(limit, MDNS_REPLY_ALL, 0));
}

void test_rate_limit_holds_a_record_back_for_one_second(void) {
  MdnsRateLimit limit;
  memset(&limit, 0, sizeof(limit));
  mdnsRateLimitRecord(&limit, MDNS_REPLY_A, 5000);

  TEST_ASSERT_EQUAL_UINT8(0, mdnsRateLimitFilter(limit, MDNS_REPLY_A, 5000));
  TEST_ASSERT_EQUAL_UINT8(0, mdnsRateLimitFilter(limit, MDNS_REPLY_A, 5999));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A, mdnsRateLimitFilter(limit, MDNS_REPLY_A, 6000));
}

void test_rate_limit_is_kept_per_record(void) {
  MdnsRateLimit limit;
  memset(&limit, 0, sizeof(limit));
  mdnsRateLimitRecord(&limit, MDNS_REPLY_A, 1000);

  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_SRV,
                          mdnsRateLimitFilter(limit, MDNS_REPLY_A | MDNS_REPLY_SRV, 1500));
}

// millis() wraps after about 49.7 days; a record sent just before the wrap is
// still held back just after it, and released a full second later.
void test_rate_limit_survives_millis_wrapping(void) {
  MdnsRateLimit limit;
  memset(&limit, 0, sizeof(limit));
  const uint32_t sentAt = 0xFFFFFE00u;
  mdnsRateLimitRecord(&limit, MDNS_REPLY_A, sentAt);

  TEST_ASSERT_EQUAL_UINT8(0, mdnsRateLimitFilter(limit, MDNS_REPLY_A, 0x00000100u));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A,
                          mdnsRateLimitFilter(limit, MDNS_REPLY_A, sentAt + 1000u));
}

void test_valid_host_labels_are_accepted(void) {
  TEST_ASSERT_TRUE(mdnsIsValidHostLabel("ourbrewbot-2924fa"));
  TEST_ASSERT_TRUE(mdnsIsValidHostLabel("a"));
  TEST_ASSERT_TRUE(mdnsIsValidHostLabel("2924fa"));
  TEST_ASSERT_TRUE(mdnsIsValidHostLabel("abcdefghijklmnopqrstuvwxyz012345"));   // 32
}

void test_invalid_host_labels_are_refused(void) {
  TEST_ASSERT_FALSE(mdnsIsValidHostLabel(""));
  TEST_ASSERT_FALSE(mdnsIsValidHostLabel("my.device"));   // would become two labels
  TEST_ASSERT_FALSE(mdnsIsValidHostLabel("-device"));
  TEST_ASSERT_FALSE(mdnsIsValidHostLabel("device-"));
  TEST_ASSERT_FALSE(mdnsIsValidHostLabel("my device"));
  TEST_ASSERT_FALSE(mdnsIsValidHostLabel("my_device"));
  TEST_ASSERT_FALSE(mdnsIsValidHostLabel("abcdefghijklmnopqrstuvwxyz0123456"));  // 33
}

void test_valid_service_names_are_accepted(void) {
  TEST_ASSERT_TRUE(mdnsIsValidServiceName("http"));
  TEST_ASSERT_TRUE(mdnsIsValidServiceName("ipp"));
  TEST_ASSERT_TRUE(mdnsIsValidServiceName("my-svc2"));
  TEST_ASSERT_TRUE(mdnsIsValidServiceName("abcdefghijklmno"));   // 15
}

void test_invalid_service_names_are_refused(void) {
  TEST_ASSERT_FALSE(mdnsIsValidServiceName(""));
  TEST_ASSERT_FALSE(mdnsIsValidServiceName("_http"));    // underscore is added for you
  TEST_ASSERT_FALSE(mdnsIsValidServiceName("-http"));
  TEST_ASSERT_FALSE(mdnsIsValidServiceName("http-"));
  TEST_ASSERT_FALSE(mdnsIsValidServiceName("ht--tp"));
  TEST_ASSERT_FALSE(mdnsIsValidServiceName("1234"));     // needs a letter
  TEST_ASSERT_FALSE(mdnsIsValidServiceName("abcdefghijklmnop"));   // 16
}

// ================================================================
// G. goodbye packets (0.4.14)
// ================================================================

// RFC 6762 section 10.1: a goodbye is every record we have given out, sent
// again with a TTL of 0. Caches drop them at once instead of holding a stale
// .local name for two minutes after a restart. Nothing else about the records
// changes, so the cache-flush bits must still be set on the unique ones.
void test_goodbye_sends_every_record_with_a_ttl_of_zero(void) {
  MdnsQueryPlan plan;
  plan.replyMask = MDNS_REPLY_ALL | MDNS_REPLY_PTR_REV;
  plan.goodbye   = true;

  uint8_t out[MDNS_TX_SIZE];
  size_t  len = mdnsBuildResponse(out, sizeof(out), plan, g_names, TEST_IP, 80, g_txt);
  Rec     recs[8];
  int     count = walkAnswers(out, len, recs, 8);
  TEST_ASSERT_EQUAL_INT(7, count);

  for (int i = 0; i < count; i++) {
    TEST_ASSERT_EQUAL_UINT32(0, recs[i].ttl);
  }
  const Rec* a = findRec(recs, count, MDNS_TYPE_A);
  TEST_ASSERT_NOT_NULL(a);
  TEST_ASSERT_EQUAL_UINT16(0x8001, a->rrClass);
}

int main(int, char**) {
  UNITY_BEGIN();

  // A. name construction
  RUN_TEST(test_names_are_built_from_the_hostname);
  RUN_TEST(test_reverse_name_reverses_the_octets);
  RUN_TEST(test_names_are_lowercased);

  // B. name decoding
  RUN_TEST(test_decode_reads_a_plain_name);
  RUN_TEST(test_decode_lowercases_as_it_reads);
  RUN_TEST(test_decode_follows_a_backward_compression_pointer);
  RUN_TEST(test_decode_rejects_a_forward_pointer);
  RUN_TEST(test_decode_rejects_a_self_pointer);
  RUN_TEST(test_decode_rejects_a_label_running_past_the_buffer);
  RUN_TEST(test_decode_rejects_an_unterminated_name);
  RUN_TEST(test_decode_refuses_to_overflow_the_output_buffer);

  // C. query parsing
  RUN_TEST(test_query_for_our_host_asks_for_the_a_record);
  RUN_TEST(test_query_is_matched_case_insensitively);
  RUN_TEST(test_query_for_another_host_asks_for_nothing);
  RUN_TEST(test_service_query_pulls_in_srv_txt_and_a);
  RUN_TEST(test_srv_query_pulls_in_the_a_record);
  RUN_TEST(test_txt_query_pulls_in_the_a_record);
  RUN_TEST(test_meta_query_enumerates_our_service_type);
  RUN_TEST(test_reverse_query_asks_for_the_reverse_ptr);
  RUN_TEST(test_any_query_for_the_host_asks_for_the_a_record);
  RUN_TEST(test_wrong_type_for_our_host_gets_an_nsec);
  RUN_TEST(test_query_in_a_foreign_class_is_ignored);
  RUN_TEST(test_unicast_bit_requests_a_direct_answer);
  RUN_TEST(test_query_from_a_legacy_port_is_answered_directly);
  RUN_TEST(test_a_response_is_not_treated_as_a_query);
  RUN_TEST(test_a_non_standard_opcode_is_refused);
  RUN_TEST(test_a_query_with_no_questions_is_refused);
  RUN_TEST(test_a_runt_packet_is_refused);
  RUN_TEST(test_answer_sections_are_never_read);
  RUN_TEST(test_a_truncated_second_question_does_not_lose_the_first);

  // D. response building
  RUN_TEST(test_nothing_is_built_for_an_empty_mask);
  RUN_TEST(test_nothing_is_built_when_it_would_not_fit);
  RUN_TEST(test_a_record_carries_our_address_in_network_order);
  RUN_TEST(test_multicast_response_has_a_zero_id_and_the_authoritative_flag);
  RUN_TEST(test_unique_records_set_the_cache_flush_bit);
  RUN_TEST(test_the_service_ptr_is_shared_not_unique);
  RUN_TEST(test_the_service_ptr_points_at_our_instance);
  RUN_TEST(test_srv_carries_the_port_and_the_host_as_target);
  RUN_TEST(test_empty_txt_is_a_single_zero_byte);
  RUN_TEST(test_an_announcement_carries_every_published_record);
  RUN_TEST(test_the_answer_count_matches_the_records_written);
  RUN_TEST(test_legacy_response_echoes_the_id_and_drops_the_cache_flush_bit);
  RUN_TEST(test_a_service_query_round_trips_into_a_complete_answer);

  // E. library configuration
  RUN_TEST(test_host_only_names_have_no_service);
  RUN_TEST(test_host_only_ignores_service_and_meta_queries);
  RUN_TEST(test_host_only_root_query_asks_for_nothing);
  RUN_TEST(test_host_only_announcement_carries_only_the_address_records);
  RUN_TEST(test_a_custom_service_type_is_answered_instead_of_http);
  RUN_TEST(test_service_names_are_lowercased);
  RUN_TEST(test_srv_carries_a_custom_port);
  RUN_TEST(test_txt_entries_are_sent_length_prefixed);
  RUN_TEST(test_txt_add_rejects_an_empty_entry);
  RUN_TEST(test_txt_add_refuses_an_entry_that_does_not_fit);
  RUN_TEST(test_txt_add_refuses_an_entry_longer_than_the_whole_buffer);
  RUN_TEST(test_the_largest_response_fits_the_transmit_buffer);

  // F. standards compliance: NSEC, legacy echo, rate limit, name rules
  RUN_TEST(test_nsec_says_only_an_a_record_exists);
  RUN_TEST(test_aaaa_query_for_our_host_gets_only_an_nsec);
  RUN_TEST(test_a_multicast_response_never_carries_a_question);
  RUN_TEST(test_legacy_response_repeats_the_question);
  RUN_TEST(test_legacy_echo_is_the_first_question_we_can_answer);
  RUN_TEST(test_legacy_aaaa_query_gets_no_answer);
  RUN_TEST(test_rate_limit_allows_a_record_never_sent_even_at_time_zero);
  RUN_TEST(test_rate_limit_holds_a_record_back_for_one_second);
  RUN_TEST(test_rate_limit_is_kept_per_record);
  RUN_TEST(test_rate_limit_survives_millis_wrapping);
  RUN_TEST(test_valid_host_labels_are_accepted);
  RUN_TEST(test_invalid_host_labels_are_refused);
  RUN_TEST(test_valid_service_names_are_accepted);
  RUN_TEST(test_invalid_service_names_are_refused);

  // G. goodbye packets
  RUN_TEST(test_goodbye_sends_every_record_with_a_ttl_of_zero);

  return UNITY_END();
}
