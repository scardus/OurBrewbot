// Native (host) tests for the staggered brew-service report dispatcher in
// OurBrewbot/Reports.cpp: queueReports(), reportsPending(),
// processReportQueue() and the payload reportBrewService() builds.
//
// Reports.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test. Unlike the other suites this one
// also pulls in Temperatures.cpp and Fermenter.cpp, because the payload's
// readings and unit conversions come from those (getBeerTemp/getAmbientTemp/
// getCurrentSG/toDisplayTemp) - stubbing them would have meant re-implementing
// the very conversions these tests are meant to check. All three compile
// against the same test/stubs/ used elsewhere, plus three new stand-ins for
// the network layer (ESP8266WiFi.h, ESP8266HTTPClient.h, WiFiClient.h) whose
// HTTPClient records the URL and body instead of sending them.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../OurBrewbot/Config.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
FermenterConfig   g_fermenters[MAX_FERMENTERS];
ProbeConfig       g_probes[MAX_PROBES];
SmartPlugConfig   g_smartPlugs[MAX_SMART_PLUGS];
TiltConfig        g_tilts[MAX_TILTS];
iSpindelConfig    g_iSpindels[MAX_ISPINDELS];
BrewServiceConfig g_brewServices[MAX_BREW_SERVICES];
GlobalConfig      g_globalConfig;
bool g_fermenterDebugMode = false;
FermenterDebugOverride g_fermenterDebugOverrides[MAX_FERMENTERS];

// ---- millis(), settable per test ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op stubs ----
void logMsgImpl(uint8_t, PGM_P, ...) {}
bool saveFermenterConfig() { return true; }
bool saveProbeConfig()     { return true; }
// Referenced inside Fermenter.cpp's processFermenters()/setFermenterPlugs(),
// neither of which these tests call - the symbols still have to resolve.
void smartPlugSwitch(uint8_t, bool) {}
void processProfiles() {}

// The code under test, plus the reading/conversion sources its payload uses.
#include "../../OurBrewbot/Temperatures.cpp"
#include "../../OurBrewbot/Fermenter.cpp"
#include "../../OurBrewbot/Reports.cpp"

// ---- test fixture ----

static const uint8_t F   = 0;   // fermenter used by most tests
static const uint8_t BF  = 0;   // service slot 0 = Brewer's Friend
static const uint8_t BFR = 1;   // service slot 1 = Brewfather

void setUp(void) {
  for (int i = 0; i < MAX_FERMENTERS; i++) g_fermenters[i] = FermenterConfig{};
  for (int i = 0; i < MAX_PROBES; i++)     g_probes[i]     = ProbeConfig{};
  for (int i = 0; i < MAX_TILTS; i++)      g_tilts[i]      = TiltConfig{};
  for (int i = 0; i < MAX_ISPINDELS; i++)  g_iSpindels[i]  = iSpindelConfig{};
  for (int s = 0; s < MAX_BREW_SERVICES; s++) g_brewServices[s] = BrewServiceConfig{};
  g_fermenterDebugMode = false;
  for (int i = 0; i < MAX_FERMENTERS; i++) g_fermenterDebugOverrides[i] = FermenterDebugOverride{};
  g_globalConfig = GlobalConfig{};
  g_globalConfig.unit = UNIT_CELSIUS;
  // Pending-report bitmaps are file-static in Reports.cpp (reachable because
  // it's #included) - a leftover bit would post in the wrong test.
  memset(s_pendingReports, 0, sizeof(s_pendingReports));
  WiFi.connected = true;
  WiFi.rssi      = -60;
  httpTestReset();
  s_millis = 0;
}

void tearDown(void) {}

// Enable a service slot with an id, which is what queueReports() requires.
static void enableService(uint8_t slot, const char* id) {
  g_brewServices[slot].enabled = true;
  strlcpy(g_brewServices[slot].serviceId, id, sizeof(g_brewServices[slot].serviceId));
  strlcpy(g_brewServices[slot].deviceName, "OurBrewbot", sizeof(g_brewServices[slot].deviceName));
}

// A powered fermenter subscribed to one service slot.
static void subscribeFermenter(uint8_t i, uint8_t slot) {
  g_fermenters[i].power        = true;
  g_fermenters[i].brewServices = (1 << slot);
}

// Assign a DS18B20 probe to a fermenter with a reading, the normal source for
// the beer/ambient temperatures in the payload.
static void setProbe(uint8_t slot, uint8_t fermenter, uint8_t function, float tempC) {
  // 20 bytes to match ProbeConfig::address, and the other two test files that
  // build an address this way. %02u is a MINIMUM width, so with slot being a
  // uint8_t the compiler has to assume three digits and a 17-byte buffer
  // warns. No caller passes anything near that - MAX_PROBES is 8 - but
  // sizing it like the field it is copied into costs nothing.
  char addr[20];
  snprintf(addr, sizeof(addr), "00000000000000%02u", slot);
  strlcpy(g_probes[slot].address, addr, sizeof(g_probes[slot].address));
  g_probes[slot].fermenter   = fermenter;
  g_probes[slot].function    = function;
  g_probes[slot].temperature = tempC;
}

static bool bodyHas(const char* fragment) {
  return strstr(g_httpTest.body, fragment) != nullptr;
}

// ---- queueReports: eligibility ----

void test_queue_ignores_disabled_service(void) {
  strlcpy(g_brewServices[BF].serviceId, "abc", sizeof(g_brewServices[BF].serviceId));
  g_brewServices[BF].enabled = false;
  subscribeFermenter(F, BF);
  queueReports();
  TEST_ASSERT_FALSE(reportsPending());
}

void test_queue_ignores_service_without_an_id(void) {
  g_brewServices[BF].enabled = true;   // enabled but never configured
  subscribeFermenter(F, BF);
  queueReports();
  TEST_ASSERT_FALSE(reportsPending());
}

void test_queue_ignores_powered_off_fermenter(void) {
  enableService(BF, "abc");
  g_fermenters[F].power        = false;
  g_fermenters[F].brewServices = (1 << BF);
  queueReports();
  TEST_ASSERT_FALSE(reportsPending());
}

void test_queue_ignores_fermenter_not_subscribed_to_the_service(void) {
  // Both services configured, fermenter subscribed to Brewfather only - the
  // brewServices bitmask must gate per service, not just per fermenter.
  enableService(BF,  "bf-key");
  enableService(BFR, "stream-id");
  g_fermenters[F].power        = true;
  g_fermenters[F].brewServices = (1 << BFR);
  queueReports();

  processReportQueue();
  TEST_ASSERT_EQUAL_INT(1, g_httpTest.postCount);
  TEST_ASSERT_EQUAL_STRING("http://log.brewfather.net/stream?id=stream-id", g_httpTest.url);
  TEST_ASSERT_FALSE(reportsPending());   // nothing queued for Brewer's Friend
}

void test_queue_accepts_an_eligible_pair(void) {
  enableService(BF, "abc");
  subscribeFermenter(F, BF);
  queueReports();
  TEST_ASSERT_TRUE(reportsPending());
}

// ---- processReportQueue: one POST per pass ----

void test_queue_drains_exactly_one_post_per_pass(void) {
  // The whole point of the staggered dispatcher: each blocking POST can take
  // up to the 5 s HTTP timeout, so only one may run per loop pass.
  enableService(BF, "abc");
  enableService(BFR, "def");
  for (int i = 0; i < 3; i++) {
    g_fermenters[i].power        = true;
    g_fermenters[i].brewServices = (1 << BF) | (1 << BFR);
  }
  queueReports();

  int expected = 0;
  for (int pass = 0; pass < 6; pass++) {   // 3 fermenters x 2 services
    processReportQueue();
    expected++;
    TEST_ASSERT_EQUAL_INT(expected, g_httpTest.postCount);
  }
  TEST_ASSERT_FALSE(reportsPending());     // queue emptied

  processReportQueue();                    // nothing left to send
  TEST_ASSERT_EQUAL_INT(6, g_httpTest.postCount);
}

void test_queue_dropped_entirely_when_wifi_is_down(void) {
  // Matches the pre-stagger behaviour: a cycle without WiFi is skipped, not
  // retried later, so reports can't pile up while the link is down.
  enableService(BF, "abc");
  subscribeFermenter(F, BF);
  queueReports();
  WiFi.connected = false;

  processReportQueue();

  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);
  TEST_ASSERT_FALSE(reportsPending());
}

// ---- reportBrewService: the payload ----

void test_payload_omits_readings_that_are_unavailable(void) {
  // With no probe/Tilt/iSpindel assigned, beer and ambient resolve to the
  // TEMP_NONE sentinel - sending that would draw -127 on a cloud graph.
  enableService(BF, "abc");
  subscribeFermenter(F, BF);
  reportBrewService(F, BF);
  // Anchor on a field that is always present, so this can't pass just because
  // the body was never captured.
  TEST_ASSERT_TRUE(bodyHas("\"temp_unit\":"));
  TEST_ASSERT_FALSE(bodyHas("\"temp\":"));
  TEST_ASSERT_FALSE(bodyHas("\"ambient\":"));
}

void test_payload_includes_readings_when_available(void) {
  enableService(BF, "abc");
  subscribeFermenter(F, BF);
  setProbe(0, F, PROBE_FN_BEER,    20.5f);
  setProbe(1, F, PROBE_FN_AMBIENT,  4.0f);

  reportBrewService(F, BF);

  TEST_ASSERT_TRUE(bodyHas("\"temp\":20.5"));
  TEST_ASSERT_TRUE(bodyHas("\"ambient\":4"));
  TEST_ASSERT_TRUE(bodyHas("\"temp_unit\":\"C\""));
}

void test_payload_converts_temperatures_to_display_unit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  enableService(BF, "abc");
  subscribeFermenter(F, BF);
  setProbe(0, F, PROBE_FN_BEER, 20.0f);   // 68 F
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;    // midpoint 20 C -> 68 F

  reportBrewService(F, BF);

  TEST_ASSERT_TRUE(bodyHas("\"temp\":68"));
  TEST_ASSERT_TRUE(bodyHas("\"temp_target\":68"));
  TEST_ASSERT_TRUE(bodyHas("\"temp_unit\":\"F\""));
}

void test_payload_target_is_the_midpoint_of_the_band(void) {
  enableService(BF, "abc");
  subscribeFermenter(F, BF);
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;
  reportBrewService(F, BF);
  TEST_ASSERT_TRUE(bodyHas("\"temp_target\":20"));
}

void test_payload_maps_controller_status_to_state(void) {
  enableService(BF, "abc");
  subscribeFermenter(F, BF);

  g_fermenters[F].status = STATUS_HEATING;
  reportBrewService(F, BF);
  TEST_ASSERT_TRUE(bodyHas("\"heat_state\":\"heating\""));

  g_fermenters[F].status = STATUS_COOLING;
  reportBrewService(F, BF);
  TEST_ASSERT_TRUE(bodyHas("\"heat_state\":\"cooling\""));
}

void test_payload_idle_state_differs_per_service(void) {
  // Long-standing quirk preserved by the service table: Brewer's Friend calls
  // idle "off", Brewfather calls it "on".
  enableService(BF, "abc");
  enableService(BFR, "def");
  subscribeFermenter(F, BF);
  g_fermenters[F].status = STATUS_IDLE;

  reportBrewService(F, BF);
  TEST_ASSERT_TRUE(bodyHas("\"heat_state\":\"off\""));

  reportBrewService(F, BFR);
  TEST_ASSERT_TRUE(bodyHas("\"device_state\":\"on\""));
}

void test_payload_uses_per_service_key_names(void) {
  enableService(BFR, "def");
  subscribeFermenter(F, BFR);
  setProbe(1, F, PROBE_FN_AMBIENT, 4.0f);

  reportBrewService(F, BFR);

  // Brewfather's names for the same two fields.
  TEST_ASSERT_TRUE(bodyHas("\"aux_temp\":4"));
  TEST_ASSERT_TRUE(bodyHas("\"rssi\":-60"));
  TEST_ASSERT_FALSE(bodyHas("\"ambient\":"));
  TEST_ASSERT_FALSE(bodyHas("\"RSSI\":"));
}

void test_payload_sends_og_only_to_brewers_friend(void) {
  enableService(BF, "abc");
  enableService(BFR, "def");
  subscribeFermenter(F, BF);
  g_fermenters[F].og = 1.050f;

  reportBrewService(F, BF);
  TEST_ASSERT_TRUE(bodyHas("\"og\":1.05"));

  reportBrewService(F, BFR);
  TEST_ASSERT_TRUE(bodyHas("\"device_state\":"));   // body really was rebuilt
  TEST_ASSERT_FALSE(bodyHas("\"og\":"));
}

void test_payload_names_the_fermenter_not_the_device(void) {
  // The service rate-limits per "name", so each fermenter reports under its
  // own name and the device name goes in device_source.
  enableService(BF, "abc");
  subscribeFermenter(F, BF);
  strlcpy(g_fermenters[F].fermenterName, "Cold Side", sizeof(g_fermenters[F].fermenterName));
  strlcpy(g_brewServices[BF].deviceName, "Shed Controller", sizeof(g_brewServices[BF].deviceName));

  reportBrewService(F, BF);

  TEST_ASSERT_TRUE(bodyHas("\"name\":\"Cold Side\""));
  TEST_ASSERT_TRUE(bodyHas("\"device_source\":\"Shed Controller\""));
}

void test_payload_url_is_built_per_service(void) {
  enableService(BF, "bf-key");
  enableService(BFR, "stream-id");
  subscribeFermenter(F, BF);

  reportBrewService(F, BF);
  TEST_ASSERT_EQUAL_STRING("http://log.brewersfriend.com/stream/bf-key", g_httpTest.url);

  reportBrewService(F, BFR);
  TEST_ASSERT_EQUAL_STRING("http://log.brewfather.net/stream?id=stream-id", g_httpTest.url);
}

void test_report_skipped_for_unconfigured_or_bad_slot(void) {
  reportBrewService(F, MAX_BREW_SERVICES);   // out of range
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);
  reportBrewService(F, BF);                  // configured slot, but no serviceId
  TEST_ASSERT_EQUAL_INT(0, g_httpTest.postCount);
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_queue_ignores_disabled_service);
  RUN_TEST(test_queue_ignores_service_without_an_id);
  RUN_TEST(test_queue_ignores_powered_off_fermenter);
  RUN_TEST(test_queue_ignores_fermenter_not_subscribed_to_the_service);
  RUN_TEST(test_queue_accepts_an_eligible_pair);

  RUN_TEST(test_queue_drains_exactly_one_post_per_pass);
  RUN_TEST(test_queue_dropped_entirely_when_wifi_is_down);

  RUN_TEST(test_payload_omits_readings_that_are_unavailable);
  RUN_TEST(test_payload_includes_readings_when_available);
  RUN_TEST(test_payload_converts_temperatures_to_display_unit);
  RUN_TEST(test_payload_target_is_the_midpoint_of_the_band);
  RUN_TEST(test_payload_maps_controller_status_to_state);
  RUN_TEST(test_payload_idle_state_differs_per_service);
  RUN_TEST(test_payload_uses_per_service_key_names);
  RUN_TEST(test_payload_sends_og_only_to_brewers_friend);
  RUN_TEST(test_payload_names_the_fermenter_not_the_device);
  RUN_TEST(test_payload_url_is_built_per_service);
  RUN_TEST(test_report_skipped_for_unconfigured_or_bad_slot);

  return UNITY_END();
}
