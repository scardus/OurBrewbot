// Native (host) tests for OurBrewbot/Temperatures.cpp: the DS18B20 probe
// stack (address encoding, bus scanning, duplicate cleanup, the two-phase
// poll) and the beer/ambient/control priority chains and unit conversions.
//
// Temperatures.cpp is #included directly (not linked) so the real,
// unmodified production source is what's under test - see test/stubs/ for
// the minimal Arduino-core stand-ins, and below for the storage the rest of
// the firmware would normally provide.
//
// The probe half of this file only became reachable once
// test/stubs/DallasTemperature.h gained a scripted device table; before
// that, getDeviceCount() was hardcoded to 0 and getTempC() to -127, so
// scanBuses(), readTempResults(), periodicProbeScan() and getTempQuick()
// compiled without ever executing a meaningful line. Everything they do -
// registering probes, renaming them when they move bus, upgrading addresses
// persisted in a truncated form, falling back to the other bus, retrying a
// first failed read, counting failures to the inactive threshold - mutates
// config that then gets written to flash, so a silent bug there is
// persistent rather than transient.

#include <unity.h>
#include <cstdint>

#include "../../OurBrewbot/Config.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
ProbeConfig      g_probes[MAX_PROBES];
TiltConfig       g_tilts[MAX_TILTS];
iSpindelConfig   g_iSpindels[MAX_ISPINDELS];
GlobalConfig     g_globalConfig;
bool g_fermenterDebugMode = false;
FermenterDebugOverride g_fermenterDebugOverrides[MAX_FERMENTERS];

// ---- millis(), settable per test (not used by the functions under test,
// kept for consistency with the other native test files) ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op stubs ----
void logMsgImpl(uint8_t, PGM_P, ...) {}

// A call counter rather than a no-op: periodicProbeScan() is supposed to
// write to flash only when the scan actually changed something, and no
// value assertion would ever notice a save on every single pass.
static int s_saveProbeConfigCalls = 0;
bool saveProbeConfig() { s_saveProbeConfigCalls++; return true; }

// The functions under test, plus everything else in Temperatures.cpp.
#include "../../OurBrewbot/Temperatures.cpp"

// ---- test fixture ----

static const uint8_t F = 0;  // fermenter index used by every test

// Real DS18B20 addresses always start 0x28 (the DS18B20 family code). B is A
// with a different serial; C is a different family byte layout entirely.
static const char* const ADDR_A = "28ff641f8b2c0011";
static const char* const ADDR_B = "28ff641f8b2c0022";
static const char* const ADDR_C = "28aabbccddee0033";

// Mirrors initDefaultProbeConfig() in Config.cpp, which is the state the
// firmware boots into. Deliberately NOT ProbeConfig{}: that zeroes function
// and fermenter, and 0 is not the "unassigned" marker - PROBE_UNASSIGNED is
// 99. cleanupDuplicateProbes() branches on exactly that comparison, so a
// zero-initialised fixture would make every slot look assigned.
static void resetProbesToDefaults() {
  for (int i = 0; i < MAX_PROBES; i++) {
    g_probes[i] = ProbeConfig{};
    strlcpy(g_probes[i].probeName, "Probe", sizeof(g_probes[i].probeName));
    g_probes[i].address[0] = '\0';
    g_probes[i].function   = PROBE_UNASSIGNED;
    g_probes[i].fermenter  = PROBE_UNASSIGNED;
    g_probes[i].busId      = 0;   // 0 = never seen on a scan
    g_probes[i].failCount  = 0;
  }
}

// Put a known probe in a slot as though a previous scan had registered it.
static void configureProbe(int slot, const char* addr, const char* name, uint8_t busId) {
  strlcpy(g_probes[slot].address, addr, sizeof(g_probes[slot].address));
  strlcpy(g_probes[slot].probeName, name, sizeof(g_probes[slot].probeName));
  g_probes[slot].busId = busId;
}

void setUp(void) {
  resetProbesToDefaults();
  for (int i = 0; i < MAX_TILTS; i++)     g_tilts[i]     = TiltConfig{};
  for (int i = 0; i < MAX_ISPINDELS; i++) g_iSpindels[i] = iSpindelConfig{};
  g_fermenterDebugMode = false;
  for (int i = 0; i < MAX_FERMENTERS; i++) g_fermenterDebugOverrides[i] = FermenterDebugOverride{};
  g_globalConfig = GlobalConfig{};
  g_globalConfig.resolution = 11;   // Config.cpp's default

  g_sensors1.testReset();
  g_sensors2.testReset();
  s_saveProbeConfigCalls = 0;
}

void tearDown(void) {}

// ============================================================
// ADDRESS UTILITIES
// ============================================================

void test_addressToString_formats_all_eight_bytes_lowercase(void) {
  DeviceAddress addr = {0x28, 0xff, 0x64, 0x1f, 0x8b, 0x2c, 0x00, 0x11};
  TEST_ASSERT_EQUAL_STRING(ADDR_A, addressToString(addr).c_str());
}

void test_addressToString_zero_pads_each_byte(void) {
  // A byte below 0x10 must still occupy two characters, or the string
  // shortens and stops matching the persisted address.
  DeviceAddress addr = {0x28, 0x08, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01};
  TEST_ASSERT_EQUAL_STRING("2808000a00000001", addressToString(addr).c_str());
}

void test_stringToAddress_round_trips(void) {
  DeviceAddress addr;
  TEST_ASSERT_TRUE(stringToAddress(ADDR_A, addr));
  TEST_ASSERT_EQUAL_STRING(ADDR_A, addressToString(addr).c_str());
}

void test_stringToAddress_decodes_the_expected_bytes(void) {
  DeviceAddress addr;
  TEST_ASSERT_TRUE(stringToAddress("28ff641f8b2c0011", addr));
  TEST_ASSERT_EQUAL_UINT8(0x28, addr[0]);
  TEST_ASSERT_EQUAL_UINT8(0xff, addr[1]);
  TEST_ASSERT_EQUAL_UINT8(0x8b, addr[4]);
  TEST_ASSERT_EQUAL_UINT8(0x11, addr[7]);
}

void test_stringToAddress_accepts_uppercase(void) {
  DeviceAddress upper, lower;
  TEST_ASSERT_TRUE(stringToAddress("28FF641F8B2C0011", upper));
  TEST_ASSERT_TRUE(stringToAddress(ADDR_A, lower));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(lower, upper, 8);
}

void test_stringToAddress_rejects_null(void) {
  DeviceAddress addr;
  TEST_ASSERT_FALSE(stringToAddress(nullptr, addr));
}

void test_stringToAddress_rejects_a_truncated_address(void) {
  // The 15-character form is exactly what the old firmware persisted, and
  // scanBuses() upgrades it rather than reading it - see the upgrade test.
  DeviceAddress addr;
  TEST_ASSERT_FALSE(stringToAddress("28ff641f8b2c001", addr));
}

void test_stringToAddress_ignores_trailing_characters(void) {
  DeviceAddress withTail, plain;
  TEST_ASSERT_TRUE(stringToAddress("28ff641f8b2c0011ZZZZ", withTail));
  TEST_ASSERT_TRUE(stringToAddress(ADDR_A, plain));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(plain, withTail, 8);
}

// ============================================================
// scanBuses
// ============================================================

void test_scanBuses_reports_no_change_with_no_probes(void) {
  TEST_ASSERT_FALSE(scanBuses());
  TEST_ASSERT_EQUAL_STRING("", g_probes[0].address);
}

void test_scanBuses_begins_both_buses(void) {
  scanBuses();
  TEST_ASSERT_EQUAL_INT(1, g_sensors1.testBeginCount());
  TEST_ASSERT_EQUAL_INT(1, g_sensors2.testBeginCount());
}

void test_scanBuses_registers_a_new_probe_on_bus1(void) {
  g_sensors1.testAddDevice(ADDR_A, 20.0f);

  TEST_ASSERT_TRUE(scanBuses());
  TEST_ASSERT_EQUAL_STRING(ADDR_A, g_probes[0].address);
  TEST_ASSERT_EQUAL_STRING("Probe Bus1-1", g_probes[0].probeName);
  TEST_ASSERT_EQUAL_UINT8(1, g_probes[0].busId);
}

void test_scanBuses_leaves_a_new_probe_unassigned(void) {
  // A newly discovered probe must not start controlling anything.
  g_sensors1.testAddDevice(ADDR_A, 20.0f);
  scanBuses();
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_probes[0].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_probes[0].fermenter);
}

void test_scanBuses_names_probes_by_position(void) {
  g_sensors1.testAddDevice(ADDR_A, 20.0f);
  g_sensors1.testAddDevice(ADDR_B, 21.0f);

  scanBuses();
  TEST_ASSERT_EQUAL_STRING("Probe Bus1-1", g_probes[0].probeName);
  TEST_ASSERT_EQUAL_STRING("Probe Bus1-2", g_probes[1].probeName);
}

void test_scanBuses_names_and_tags_bus2_probes_separately(void) {
  g_sensors2.testAddDevice(ADDR_C, 5.0f);

  TEST_ASSERT_TRUE(scanBuses());
  TEST_ASSERT_EQUAL_STRING(ADDR_C, g_probes[0].address);
  TEST_ASSERT_EQUAL_STRING("Probe Bus2-1", g_probes[0].probeName);
  TEST_ASSERT_EQUAL_UINT8(2, g_probes[0].busId);
}

void test_scanBuses_registers_both_buses_in_one_pass(void) {
  g_sensors1.testAddDevice(ADDR_A, 20.0f);
  g_sensors1.testAddDevice(ADDR_B, 21.0f);
  g_sensors2.testAddDevice(ADDR_C, 5.0f);

  TEST_ASSERT_TRUE(scanBuses());
  TEST_ASSERT_EQUAL_STRING(ADDR_A, g_probes[0].address);
  TEST_ASSERT_EQUAL_STRING(ADDR_B, g_probes[1].address);
  TEST_ASSERT_EQUAL_STRING(ADDR_C, g_probes[2].address);
  TEST_ASSERT_EQUAL_UINT8(2, g_probes[2].busId);
}

void test_scanBuses_reports_no_change_for_an_unchanged_probe(void) {
  // Already registered under the name this position produces - a rescan must
  // not report a change, or every scan would rewrite flash.
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_sensors1.testAddDevice(ADDR_A, 20.0f);

  TEST_ASSERT_FALSE(scanBuses());
  TEST_ASSERT_EQUAL_INT(1, g_probes[0].busId);
}

void test_scanBuses_renames_and_retags_a_probe_moved_to_the_other_bus(void) {
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_sensors2.testAddDevice(ADDR_A, 20.0f);   // same probe, now on bus 2

  TEST_ASSERT_TRUE(scanBuses());
  TEST_ASSERT_EQUAL_STRING("Probe Bus2-1", g_probes[0].probeName);
  TEST_ASSERT_EQUAL_UINT8(2, g_probes[0].busId);
}

void test_scanBuses_keeps_the_assignment_when_it_renames(void) {
  // Moving a probe between jacks must not silently unassign it from its
  // fermenter - only the positional name changes.
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_probes[0].function  = PROBE_FN_BEER;
  g_probes[0].fermenter = F;
  g_sensors2.testAddDevice(ADDR_A, 20.0f);

  scanBuses();
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_BEER, g_probes[0].function);
  TEST_ASSERT_EQUAL_UINT8(F, g_probes[0].fermenter);
}

void test_scanBuses_upgrades_a_truncated_address(void) {
  // Older firmware persisted a 15-character address. The scan recognises it
  // as a prefix of the real one and rewrites it in full.
  configureProbe(0, "28ff641f8b2c001", "Probe Bus1-1", 1);
  g_sensors1.testAddDevice(ADDR_A, 20.0f);

  TEST_ASSERT_TRUE(scanBuses());
  TEST_ASSERT_EQUAL_STRING(ADDR_A, g_probes[0].address);
}

void test_scanBuses_upgrade_keeps_the_assignment(void) {
  configureProbe(0, "28ff641f8b2c001", "Probe Bus1-1", 1);
  g_probes[0].function   = PROBE_FN_AMBIENT;
  g_probes[0].fermenter  = F;
  g_probes[0].tempAdjust = 0.4f;
  g_sensors1.testAddDevice(ADDR_A, 20.0f);

  scanBuses();
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_AMBIENT, g_probes[0].function);
  TEST_ASSERT_EQUAL_UINT8(F, g_probes[0].fermenter);
  TEST_ASSERT_EQUAL_FLOAT(0.4f, g_probes[0].tempAdjust);
}

void test_scanBuses_does_not_register_a_second_copy_after_an_upgrade(void) {
  configureProbe(0, "28ff641f8b2c001", "Probe Bus1-1", 1);
  g_sensors1.testAddDevice(ADDR_A, 20.0f);

  scanBuses();
  TEST_ASSERT_EQUAL_STRING("", g_probes[1].address);
}

void test_scanBuses_ignores_probes_beyond_the_slot_limit(void) {
  // Every slot already taken by a probe that isn't on either bus, so the new
  // one has nowhere to go. It must be dropped, not written past the array.
  for (int i = 0; i < MAX_PROBES; i++) {
    char addr[20];
    snprintf(addr, sizeof(addr), "28000000000000%02x", i + 1);
    configureProbe(i, addr, "Probe", 1);
  }
  g_sensors1.testAddDevice(ADDR_A, 20.0f);

  TEST_ASSERT_FALSE(scanBuses());
  for (int i = 0; i < MAX_PROBES; i++) {
    TEST_ASSERT_FALSE(strcasecmp(g_probes[i].address, ADDR_A) == 0);
  }
}

// ============================================================
// requestTempConversion - phase 1 of the async poll
// ============================================================

void test_requestTempConversion_requests_on_both_buses(void) {
  requestTempConversion();
  TEST_ASSERT_EQUAL_INT(1, g_sensors1.testRequestCount());
  TEST_ASSERT_EQUAL_INT(1, g_sensors2.testRequestCount());
}

void test_requestTempConversion_does_not_block(void) {
  // The whole point of the two-phase split: loop() must not stall for the
  // ~750 ms conversion. A blocking request here would stall every cycle.
  requestTempConversion();
  TEST_ASSERT_FALSE(g_sensors1.testLastRequestBlocking());
  TEST_ASSERT_FALSE(g_sensors2.testLastRequestBlocking());
}

// ============================================================
// readTempResults - phase 2 of the async poll
// ============================================================

void test_readTempResults_stores_the_reading(void) {
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_sensors1.testAddDevice(ADDR_A, 20.5f);

  readTempResults();
  TEST_ASSERT_EQUAL_FLOAT(20.5f, g_probes[0].rawTemperature);
  TEST_ASSERT_EQUAL_FLOAT(20.5f, g_probes[0].temperature);
}

void test_readTempResults_applies_the_calibration_offset(void) {
  // rawTemperature is what the probe said; temperature is what the firmware
  // acts on. The offset must land on one and not the other.
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_probes[0].tempAdjust = -0.7f;
  g_sensors1.testAddDevice(ADDR_A, 20.0f);

  readTempResults();
  TEST_ASSERT_EQUAL_FLOAT(20.0f, g_probes[0].rawTemperature);
  TEST_ASSERT_EQUAL_FLOAT(19.3f, g_probes[0].temperature);
}

void test_readTempResults_skips_empty_slots(void) {
  g_sensors1.testAddDevice(ADDR_A, 20.5f);
  readTempResults();
  TEST_ASSERT_EQUAL_INT(0, g_sensors1.testReadCount());
}

void test_readTempResults_skips_an_unparseable_address(void) {
  configureProbe(0, "28ff64", "Probe Bus1-1", 1);
  g_sensors1.testAddDevice(ADDR_A, 20.5f);

  readTempResults();
  TEST_ASSERT_EQUAL_INT(0, g_sensors1.testReadCount());
  TEST_ASSERT_EQUAL_UINT8(0, g_probes[0].failCount);
}

void test_readTempResults_reads_a_bus2_probe_from_bus2(void) {
  configureProbe(0, ADDR_C, "Probe Bus2-1", 2);
  g_sensors2.testAddDevice(ADDR_C, 5.0f);

  readTempResults();
  TEST_ASSERT_EQUAL_FLOAT(5.0f, g_probes[0].temperature);
  TEST_ASSERT_EQUAL_INT(0, g_sensors1.testReadCount());
}

void test_readTempResults_falls_back_to_bus2_when_never_scanned(void) {
  // busId 0 means no scan has placed this probe yet - the read has to try
  // both buses rather than assume bus 1.
  configureProbe(0, ADDR_C, "Probe", 0);
  g_sensors2.testAddDevice(ADDR_C, 5.0f);

  readTempResults();
  TEST_ASSERT_EQUAL_FLOAT(5.0f, g_probes[0].temperature);
}

void test_readTempResults_remembers_the_bus_that_answered(void) {
  // Without the write-back, every later cycle would pay the failed bus-1
  // read again.
  configureProbe(0, ADDR_C, "Probe", 0);
  g_sensors2.testAddDevice(ADDR_C, 5.0f);

  readTempResults();
  TEST_ASSERT_EQUAL_UINT8(2, g_probes[0].busId);
}

void test_readTempResults_retries_a_first_failure_and_recovers(void) {
  // One transient miss on a marginal connection must not count as a failure.
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_sensors1.testAddDevice(ADDR_A, 20.5f);
  g_sensors1.testSetFailures(ADDR_A, 1);

  readTempResults();
  TEST_ASSERT_EQUAL_FLOAT(20.5f, g_probes[0].temperature);
  TEST_ASSERT_EQUAL_UINT8(0, g_probes[0].failCount);
  TEST_ASSERT_EQUAL_INT(2, g_sensors1.testReadCount());
}

void test_readTempResults_retry_requests_a_fresh_blocking_conversion(void) {
  // Re-reading the same stale scratchpad would return the same failure, so
  // the retry has to request a conversion and wait for it.
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_sensors1.testAddDevice(ADDR_A, 20.5f);
  g_sensors1.testSetFailures(ADDR_A, 1);

  readTempResults();
  TEST_ASSERT_EQUAL_INT(1, g_sensors1.testRequestCount());
  TEST_ASSERT_TRUE(g_sensors1.testLastRequestBlocking());
}

void test_readTempResults_leaves_the_bus_non_blocking_after_a_retry(void) {
  // If the retry left waitForConversion set, the next requestTempConversion()
  // would stall the main loop for the full conversion time.
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_sensors1.testAddDevice(ADDR_A, 20.5f);
  g_sensors1.testSetFailures(ADDR_A, 1);

  readTempResults();
  TEST_ASSERT_FALSE(g_sensors1.testWaitForConversion());
}

void test_readTempResults_only_retries_the_first_failure(void) {
  // failCount already non-zero: the probe is known bad, so the cycle must
  // not spend another 750 ms on it.
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_probes[0].failCount = 1;
  g_sensors1.testAddDevice(ADDR_A, 20.5f);
  g_sensors1.testSetFailures(ADDR_A, 99);

  readTempResults();
  TEST_ASSERT_EQUAL_INT(1, g_sensors1.testReadCount());
  TEST_ASSERT_EQUAL_INT(0, g_sensors1.testRequestCount());
}

void test_readTempResults_counts_a_persistent_failure(void) {
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_probes[0].failCount = 2;
  g_sensors1.testAddDevice(ADDR_A, 20.5f);
  g_sensors1.testSetFailures(ADDR_A, 99);

  readTempResults();
  TEST_ASSERT_EQUAL_UINT8(3, g_probes[0].failCount);
}

void test_readTempResults_keeps_the_last_reading_below_the_threshold(void) {
  // A brief dropout shouldn't blank the reading the controller is acting on.
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_probes[0].failCount   = 1;
  g_probes[0].temperature = 20.5f;
  g_sensors1.testAddDevice(ADDR_A, 20.5f);
  g_sensors1.testSetFailures(ADDR_A, 99);

  readTempResults();
  TEST_ASSERT_EQUAL_UINT8(2, g_probes[0].failCount);
  TEST_ASSERT_EQUAL_FLOAT(20.5f, g_probes[0].temperature);
}

void test_readTempResults_marks_the_probe_inactive_at_the_threshold(void) {
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_probes[0].failCount   = PROBE_FAIL_THRESHOLD - 1;
  g_probes[0].temperature = 20.5f;
  g_sensors1.testAddDevice(ADDR_A, 20.5f);
  g_sensors1.testSetFailures(ADDR_A, 99);

  readTempResults();
  TEST_ASSERT_EQUAL_UINT8(PROBE_FAIL_THRESHOLD, g_probes[0].failCount);
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, g_probes[0].temperature);
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, g_probes[0].rawTemperature);
}

void test_readTempResults_clears_the_fail_count_on_recovery(void) {
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_probes[0].failCount = PROBE_FAIL_THRESHOLD;
  g_sensors1.testAddDevice(ADDR_A, 20.5f);

  readTempResults();
  TEST_ASSERT_EQUAL_UINT8(0, g_probes[0].failCount);
  TEST_ASSERT_EQUAL_FLOAT(20.5f, g_probes[0].temperature);
}

void test_readTempResults_handles_every_configured_probe(void) {
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  configureProbe(1, ADDR_C, "Probe Bus2-1", 2);
  g_sensors1.testAddDevice(ADDR_A, 20.5f);
  g_sensors2.testAddDevice(ADDR_C, 5.0f);

  readTempResults();
  TEST_ASSERT_EQUAL_FLOAT(20.5f, g_probes[0].temperature);
  TEST_ASSERT_EQUAL_FLOAT(5.0f, g_probes[1].temperature);
}

// ============================================================
// periodicProbeScan
// ============================================================

void test_periodicProbeScan_reapplies_the_resolution(void) {
  // scanBuses() calls begin(), which resets the library's resolution. If it
  // isn't re-applied, every probe silently drops to the 9-bit default.
  g_globalConfig.resolution = 11;
  periodicProbeScan();
  TEST_ASSERT_EQUAL_UINT8(11, g_sensors1.testResolution());
  TEST_ASSERT_EQUAL_UINT8(11, g_sensors2.testResolution());
}

void test_periodicProbeScan_saves_when_a_probe_was_added(void) {
  g_sensors1.testAddDevice(ADDR_A, 20.0f);
  periodicProbeScan();
  TEST_ASSERT_EQUAL_INT(1, s_saveProbeConfigCalls);
}

void test_periodicProbeScan_does_not_save_when_nothing_changed(void) {
  // This runs on a timer forever - a save per pass would write flash for the
  // life of the device.
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  g_sensors1.testAddDevice(ADDR_A, 20.0f);

  periodicProbeScan();
  TEST_ASSERT_EQUAL_INT(0, s_saveProbeConfigCalls);
}

// ============================================================
// getTempQuick
// ============================================================

void test_getTempQuick_reads_from_bus1(void) {
  g_sensors1.testAddDevice(ADDR_A, 20.5f);
  TEST_ASSERT_EQUAL_FLOAT(20.5f, getTempQuick(ADDR_A));
}

void test_getTempQuick_falls_back_to_bus2(void) {
  g_sensors2.testAddDevice(ADDR_C, 5.0f);
  TEST_ASSERT_EQUAL_FLOAT(5.0f, getTempQuick(ADDR_C));
}

void test_getTempQuick_returns_none_for_an_absent_probe(void) {
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, getTempQuick(ADDR_A));
}

void test_getTempQuick_returns_none_for_an_unparseable_address(void) {
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, getTempQuick("nonsense"));
}

// ============================================================
// cleanupDuplicateProbes
// ============================================================

void test_cleanup_removes_an_exact_duplicate(void) {
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  configureProbe(1, ADDR_A, "Probe Bus1-2", 1);

  cleanupDuplicateProbes();
  TEST_ASSERT_EQUAL_STRING(ADDR_A, g_probes[0].address);
  TEST_ASSERT_EQUAL_STRING("", g_probes[1].address);
}

void test_cleanup_keeps_the_full_address_over_the_truncated_one(void) {
  // The truncated entry is the stale one, even though it sits in the lower
  // slot - keeping it would lose four characters of the address.
  configureProbe(0, "28ff641f8b2c001", "Probe Bus1-1", 1);
  configureProbe(1, ADDR_A, "Probe Bus1-2", 1);

  cleanupDuplicateProbes();
  TEST_ASSERT_EQUAL_STRING("", g_probes[0].address);
  TEST_ASSERT_EQUAL_STRING(ADDR_A, g_probes[1].address);
}

void test_cleanup_transfers_the_assignment_from_the_removed_entry(void) {
  // The stale truncated entry is the one the user configured, so its
  // assignment has to move across or the fermenter loses its probe.
  configureProbe(0, "28ff641f8b2c001", "Beer Temp", 1);
  g_probes[0].function   = PROBE_FN_BEER;
  g_probes[0].fermenter  = F;
  g_probes[0].tempAdjust = 0.3f;
  configureProbe(1, ADDR_A, "Probe Bus1-2", 1);

  cleanupDuplicateProbes();
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_BEER, g_probes[1].function);
  TEST_ASSERT_EQUAL_UINT8(F, g_probes[1].fermenter);
  TEST_ASSERT_EQUAL_FLOAT(0.3f, g_probes[1].tempAdjust);
  TEST_ASSERT_EQUAL_STRING("Beer Temp", g_probes[1].probeName);
}

void test_cleanup_leaves_an_unassigned_duplicate_alone(void) {
  // Nothing to transfer, so the surviving entry keeps its own settings.
  configureProbe(0, ADDR_A, "Beer Temp", 1);
  g_probes[0].function  = PROBE_FN_BEER;
  g_probes[0].fermenter = F;
  configureProbe(1, ADDR_A, "Probe Bus1-2", 1);

  cleanupDuplicateProbes();
  TEST_ASSERT_EQUAL_UINT8(PROBE_FN_BEER, g_probes[0].function);
  TEST_ASSERT_EQUAL_STRING("Beer Temp", g_probes[0].probeName);
}

void test_cleanup_resets_the_removed_slot_to_unassigned(void) {
  // memset would leave function 0, which is not the "unassigned" marker.
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  configureProbe(1, ADDR_A, "Probe Bus1-2", 1);

  cleanupDuplicateProbes();
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_probes[1].function);
  TEST_ASSERT_EQUAL_UINT8(PROBE_UNASSIGNED, g_probes[1].fermenter);
}

void test_cleanup_leaves_distinct_probes_untouched(void) {
  configureProbe(0, ADDR_A, "Probe Bus1-1", 1);
  configureProbe(1, ADDR_B, "Probe Bus1-2", 1);
  configureProbe(2, ADDR_C, "Probe Bus2-1", 2);

  cleanupDuplicateProbes();
  TEST_ASSERT_EQUAL_STRING(ADDR_A, g_probes[0].address);
  TEST_ASSERT_EQUAL_STRING(ADDR_B, g_probes[1].address);
  TEST_ASSERT_EQUAL_STRING(ADDR_C, g_probes[2].address);
}

// ============================================================
// getBeerTemp / getBeerTempSource: Tilt > Probe > iSpindel > None
// ============================================================

void test_getBeerTemp_none_when_nothing_assigned(void) {
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, getBeerTemp(F));
  TEST_ASSERT_EQUAL_STRING("None", getBeerTempSource(F));
}

void test_getBeerTemp_uses_ispindel_when_assigned(void) {
  g_iSpindels[0].collectData = true;
  g_iSpindels[0].fermenter   = F;
  g_iSpindels[0].function    = PROBE_FN_BEER;
  g_iSpindels[0].temperature = 19.5f;
  TEST_ASSERT_EQUAL_FLOAT(19.5f, getBeerTemp(F));
  TEST_ASSERT_EQUAL_STRING("iSpindel", getBeerTempSource(F));
}

void test_getBeerTemp_probe_beats_ispindel(void) {
  g_iSpindels[0].collectData = true;
  g_iSpindels[0].fermenter   = F;
  g_iSpindels[0].function    = PROBE_FN_BEER;
  g_iSpindels[0].temperature = 19.5f;

  strlcpy(g_probes[0].address, "0000000000000001", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_BEER;
  g_probes[0].temperature = 20.1f;

  TEST_ASSERT_EQUAL_FLOAT(20.1f, getBeerTemp(F));
  TEST_ASSERT_EQUAL_STRING("Probe", getBeerTempSource(F));
}

void test_getBeerTemp_tilt_beats_probe(void) {
  strlcpy(g_probes[0].address, "0000000000000001", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_BEER;
  g_probes[0].temperature = 20.1f;

  g_tilts[0].active      = true;
  g_tilts[0].fermenter    = F;
  g_tilts[0].function     = PROBE_FN_BEER;
  g_tilts[0].temperature  = 18.7f;

  TEST_ASSERT_EQUAL_FLOAT(18.7f, getBeerTemp(F));
  TEST_ASSERT_EQUAL_STRING("Tilt", getBeerTempSource(F));
}

void test_getBeerTemp_debug_override_beats_everything(void) {
  g_tilts[0].active      = true;
  g_tilts[0].fermenter    = F;
  g_tilts[0].function     = PROBE_FN_BEER;
  g_tilts[0].temperature  = 18.7f;

  g_fermenterDebugMode = true;
  g_fermenterDebugOverrides[F].enabled  = true;
  g_fermenterDebugOverrides[F].beerTemp = 25.0f;

  TEST_ASSERT_EQUAL_FLOAT(25.0f, getBeerTemp(F));
  TEST_ASSERT_EQUAL_STRING("Debug", getBeerTempSource(F));
}

// ---- getAmbientTemp ----

void test_getAmbientTemp_none_when_nothing_assigned(void) {
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, getAmbientTemp(F));
}

void test_getAmbientTemp_uses_assigned_probe(void) {
  strlcpy(g_probes[0].address, "0000000000000002", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_AMBIENT;
  g_probes[0].temperature = 4.2f;
  TEST_ASSERT_EQUAL_FLOAT(4.2f, getAmbientTemp(F));
}

void test_getAmbientTemp_debug_override_beats_probe(void) {
  strlcpy(g_probes[0].address, "0000000000000002", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_AMBIENT;
  g_probes[0].temperature = 4.2f;

  g_fermenterDebugMode = true;
  g_fermenterDebugOverrides[F].enabled     = true;
  g_fermenterDebugOverrides[F].ambientTemp = 6.6f;

  TEST_ASSERT_EQUAL_FLOAT(6.6f, getAmbientTemp(F));
}

// ---- getControlTemp: beer preferred, ambient fallback ----

void test_getControlTemp_prefers_beer_when_usable(void) {
  strlcpy(g_probes[0].address, "0000000000000001", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_BEER;
  g_probes[0].temperature = 20.1f;

  strlcpy(g_probes[1].address, "0000000000000002", sizeof(g_probes[1].address));
  g_probes[1].fermenter   = F;
  g_probes[1].function    = PROBE_FN_AMBIENT;
  g_probes[1].temperature = 4.2f;

  TEST_ASSERT_EQUAL_FLOAT(20.1f, getControlTemp(F));
}

void test_getControlTemp_falls_back_to_ambient(void) {
  strlcpy(g_probes[0].address, "0000000000000002", sizeof(g_probes[0].address));
  g_probes[0].fermenter   = F;
  g_probes[0].function    = PROBE_FN_AMBIENT;
  g_probes[0].temperature = 4.2f;
  // No beer source assigned - beer resolves to TEMP_NONE.
  TEST_ASSERT_EQUAL_FLOAT(4.2f, getControlTemp(F));
}

// ---- unit conversions ----

void test_toDisplayTemp_identity_in_celsius(void) {
  g_globalConfig.unit = UNIT_CELSIUS;
  TEST_ASSERT_EQUAL_FLOAT(20.0f, toDisplayTemp(20.0f));
}

void test_toDisplayTemp_scales_in_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  TEST_ASSERT_EQUAL_FLOAT(68.0f, toDisplayTemp(20.0f));
}

void test_toCelsius_identity_in_celsius(void) {
  g_globalConfig.unit = UNIT_CELSIUS;
  TEST_ASSERT_EQUAL_FLOAT(20.0f, toCelsius(20.0f));
}

void test_toCelsius_scales_in_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  TEST_ASSERT_EQUAL_FLOAT(20.0f, toCelsius(68.0f));
}

void test_temp_roundtrip_in_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 20.0f, toCelsius(toDisplayTemp(20.0f)));
}

void test_tempDelta_scales_without_offset_in_fahrenheit(void) {
  g_globalConfig.unit = UNIT_FAHRENHEIT;
  TEST_ASSERT_EQUAL_FLOAT(1.8f, toDisplayTempDelta(1.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, toCelsiusTempDelta(1.8f));
}

void test_tempDelta_identity_in_celsius(void) {
  g_globalConfig.unit = UNIT_CELSIUS;
  TEST_ASSERT_EQUAL_FLOAT(1.0f, toDisplayTempDelta(1.0f));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, toCelsiusTempDelta(1.0f));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_addressToString_formats_all_eight_bytes_lowercase);
  RUN_TEST(test_addressToString_zero_pads_each_byte);
  RUN_TEST(test_stringToAddress_round_trips);
  RUN_TEST(test_stringToAddress_decodes_the_expected_bytes);
  RUN_TEST(test_stringToAddress_accepts_uppercase);
  RUN_TEST(test_stringToAddress_rejects_null);
  RUN_TEST(test_stringToAddress_rejects_a_truncated_address);
  RUN_TEST(test_stringToAddress_ignores_trailing_characters);

  RUN_TEST(test_scanBuses_reports_no_change_with_no_probes);
  RUN_TEST(test_scanBuses_begins_both_buses);
  RUN_TEST(test_scanBuses_registers_a_new_probe_on_bus1);
  RUN_TEST(test_scanBuses_leaves_a_new_probe_unassigned);
  RUN_TEST(test_scanBuses_names_probes_by_position);
  RUN_TEST(test_scanBuses_names_and_tags_bus2_probes_separately);
  RUN_TEST(test_scanBuses_registers_both_buses_in_one_pass);
  RUN_TEST(test_scanBuses_reports_no_change_for_an_unchanged_probe);
  RUN_TEST(test_scanBuses_renames_and_retags_a_probe_moved_to_the_other_bus);
  RUN_TEST(test_scanBuses_keeps_the_assignment_when_it_renames);
  RUN_TEST(test_scanBuses_upgrades_a_truncated_address);
  RUN_TEST(test_scanBuses_upgrade_keeps_the_assignment);
  RUN_TEST(test_scanBuses_does_not_register_a_second_copy_after_an_upgrade);
  RUN_TEST(test_scanBuses_ignores_probes_beyond_the_slot_limit);

  RUN_TEST(test_requestTempConversion_requests_on_both_buses);
  RUN_TEST(test_requestTempConversion_does_not_block);

  RUN_TEST(test_readTempResults_stores_the_reading);
  RUN_TEST(test_readTempResults_applies_the_calibration_offset);
  RUN_TEST(test_readTempResults_skips_empty_slots);
  RUN_TEST(test_readTempResults_skips_an_unparseable_address);
  RUN_TEST(test_readTempResults_reads_a_bus2_probe_from_bus2);
  RUN_TEST(test_readTempResults_falls_back_to_bus2_when_never_scanned);
  RUN_TEST(test_readTempResults_remembers_the_bus_that_answered);
  RUN_TEST(test_readTempResults_retries_a_first_failure_and_recovers);
  RUN_TEST(test_readTempResults_retry_requests_a_fresh_blocking_conversion);
  RUN_TEST(test_readTempResults_leaves_the_bus_non_blocking_after_a_retry);
  RUN_TEST(test_readTempResults_only_retries_the_first_failure);
  RUN_TEST(test_readTempResults_counts_a_persistent_failure);
  RUN_TEST(test_readTempResults_keeps_the_last_reading_below_the_threshold);
  RUN_TEST(test_readTempResults_marks_the_probe_inactive_at_the_threshold);
  RUN_TEST(test_readTempResults_clears_the_fail_count_on_recovery);
  RUN_TEST(test_readTempResults_handles_every_configured_probe);

  RUN_TEST(test_periodicProbeScan_reapplies_the_resolution);
  RUN_TEST(test_periodicProbeScan_saves_when_a_probe_was_added);
  RUN_TEST(test_periodicProbeScan_does_not_save_when_nothing_changed);

  RUN_TEST(test_getTempQuick_reads_from_bus1);
  RUN_TEST(test_getTempQuick_falls_back_to_bus2);
  RUN_TEST(test_getTempQuick_returns_none_for_an_absent_probe);
  RUN_TEST(test_getTempQuick_returns_none_for_an_unparseable_address);

  RUN_TEST(test_cleanup_removes_an_exact_duplicate);
  RUN_TEST(test_cleanup_keeps_the_full_address_over_the_truncated_one);
  RUN_TEST(test_cleanup_transfers_the_assignment_from_the_removed_entry);
  RUN_TEST(test_cleanup_leaves_an_unassigned_duplicate_alone);
  RUN_TEST(test_cleanup_resets_the_removed_slot_to_unassigned);
  RUN_TEST(test_cleanup_leaves_distinct_probes_untouched);

  RUN_TEST(test_getBeerTemp_none_when_nothing_assigned);
  RUN_TEST(test_getBeerTemp_uses_ispindel_when_assigned);
  RUN_TEST(test_getBeerTemp_probe_beats_ispindel);
  RUN_TEST(test_getBeerTemp_tilt_beats_probe);
  RUN_TEST(test_getBeerTemp_debug_override_beats_everything);

  RUN_TEST(test_getAmbientTemp_none_when_nothing_assigned);
  RUN_TEST(test_getAmbientTemp_uses_assigned_probe);
  RUN_TEST(test_getAmbientTemp_debug_override_beats_probe);

  RUN_TEST(test_getControlTemp_prefers_beer_when_usable);
  RUN_TEST(test_getControlTemp_falls_back_to_ambient);

  RUN_TEST(test_toDisplayTemp_identity_in_celsius);
  RUN_TEST(test_toDisplayTemp_scales_in_fahrenheit);
  RUN_TEST(test_toCelsius_identity_in_celsius);
  RUN_TEST(test_toCelsius_scales_in_fahrenheit);
  RUN_TEST(test_temp_roundtrip_in_fahrenheit);
  RUN_TEST(test_tempDelta_scales_without_offset_in_fahrenheit);
  RUN_TEST(test_tempDelta_identity_in_celsius);

  return UNITY_END();
}
