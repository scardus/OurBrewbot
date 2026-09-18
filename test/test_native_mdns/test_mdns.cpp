// Native (host) tests for the mDNS packet layer in OurBrewbot/MdnsPacket.cpp:
// mdnsBuildNames(), mdnsDecodeName(), mdnsParseQuery() and
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

#include "../../OurBrewbot/MdnsPacket.cpp"

// ---- fixture ----

static MdnsNames g_names;

// 192.168.0.207, in the host order this module uses (first octet = MSB).
#define TEST_IP  0xC0A800CFu

void setUp(void) {
  mdnsBuildNames("ourbrewbot-2924fa", TEST_IP, &g_names);
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
  return mdnsBuildResponse(out, outSize, plan, g_names, TEST_IP, 80);
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
  mdnsBuildNames("OurBrewBot-2924FA", TEST_IP, &n);
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
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A, plan.replyMask);
  TEST_ASSERT_FALSE(plan.unicast);
  TEST_ASSERT_FALSE(plan.legacy);
}

void test_query_is_matched_case_insensitively(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "OurBrewbot-2924FA.Local", MDNS_TYPE_A, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A, plan.replyMask);
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
                          MDNS_REPLY_TXT | MDNS_REPLY_A, plan.replyMask);
}

void test_srv_query_pulls_in_the_a_record(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa._http._tcp.local", MDNS_TYPE_SRV, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_SRV | MDNS_REPLY_A, plan.replyMask);
}

void test_txt_query_pulls_in_the_a_record(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa._http._tcp.local", MDNS_TYPE_TXT, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_TXT | MDNS_REPLY_A, plan.replyMask);
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
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A, plan.replyMask);
}

// Asking for the A record with a type we don't publish must stay silent.
void test_wrong_type_for_our_host_asks_for_nothing(void) {
  uint8_t pkt[128];
  size_t  len = buildQuery(pkt, "ourbrewbot-2924fa.local", MDNS_TYPE_SRV, 1);

  MdnsQueryPlan plan;
  TEST_ASSERT_TRUE(mdnsParseQuery(pkt, len, g_names, 5353, &plan));
  TEST_ASSERT_EQUAL_UINT8(0, plan.replyMask);
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
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A, plan.replyMask);
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
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A, plan.replyMask);
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
  TEST_ASSERT_EQUAL_UINT8(MDNS_REPLY_A, plan.replyMask);
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

  TEST_ASSERT_EQUAL_INT(5, count);
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
  size_t  len = mdnsBuildResponse(out, sizeof(out), plan, g_names, TEST_IP, 80);
  TEST_ASSERT_TRUE(len > 12);

  Rec recs[16];
  int count = walkAnswers(out, len, recs, 16);
  TEST_ASSERT_EQUAL_INT(4, count);
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_PTR));
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_SRV));
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_TXT));
  TEST_ASSERT_NOT_NULL(findRec(recs, count, MDNS_TYPE_A));
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
  RUN_TEST(test_wrong_type_for_our_host_asks_for_nothing);
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

  return UNITY_END();
}
