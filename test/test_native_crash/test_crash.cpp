// Native (host) tests for the post-mortem machinery in OurBrewbot/Crash.cpp:
// the main-loop checkpoint breadcrumb, the DEFERRED crash/checkpoint reporting
// on the next boot, and the exception callback that captures the register
// frame and a stack slice into RTC user memory.
//
// This code only ever runs on the way out of, or back from, a crash - the one
// path that cannot be exercised by hand on a live device. The stakes are also
// asymmetric: a bug in custom_crash_callback faults *inside* the exception
// handler and destroys the very diagnostic it was collecting.
//
// Crash.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test. That also puts its anonymous
// namespace - CrashRecord, the magics, MODULE_NAMES and s_lastModule - in
// scope, which is what lets the module table below be pinned directly.
//
// test/stubs/Esp.h provides the 512-byte RTC array these records live in.

// Platform headers first, for the low-address reservation explained at
// reserveFakeStack() below. NOMINMAX matters: windows.h otherwise defines
// min/max as macros and breaks the `using std::min` in test/stubs/Arduino.h.
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#else
  #include <sys/mman.h>
#endif

#include <unity.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#include "../../OurBrewbot/Crash.h"
#include "../../OurBrewbot/Log.h"

// ---- millis(), unused here but declared by the Arduino stub ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- log capture: the DEFERRED lines ARE the observable behaviour ----
#define MAX_LOG_LINES 16
#define MAX_LOG_LEN   256
static char s_logLines[MAX_LOG_LINES][MAX_LOG_LEN];
static int  s_logCount = 0;

void logMsgImpl(uint8_t, PGM_P fmt, ...) {
  if (s_logCount >= MAX_LOG_LINES) return;
  va_list args;
  va_start(args, fmt);
  vsnprintf(s_logLines[s_logCount], MAX_LOG_LEN, fmt, args);
  va_end(args);
  s_logCount++;
}

static bool logContains(const char* needle) {
  for (int i = 0; i < s_logCount; i++) {
    if (strstr(s_logLines[i], needle)) return true;
  }
  return false;
}

// The code under test.
#include "../../OurBrewbot/Crash.cpp"

// ---- fixture helpers ----

// Stage a crash record in RTC exactly as custom_crash_callback would have
// left it, so the next-boot reporting path can be tested on its own.
static void stageCrashRecord(uint32_t lastCheckpoint, uint32_t magic = CRASH_MAGIC) {
  CrashRecord rec = {};
  rec.magic          = magic;
  rec.reason         = REASON_EXCEPTION_RST;
  rec.exccause       = 28;
  rec.epc1           = 0x40201234;
  rec.epc2           = 0x40205678;
  rec.epc3           = 0x40209ABC;
  rec.depc           = 0x4020DEF0;
  rec.excvaddr       = 0x00000000;
  rec.stackStart     = 0x3FFFFC00;
  rec.stackEnd       = 0x3FFFFD00;
  rec.lastCheckpoint = lastCheckpoint;
  ESP.rtcUserMemoryWrite(CRASH_OFFSET, reinterpret_cast<uint32_t*>(&rec), sizeof(rec));
}

static CrashRecord readCrashRecord() {
  CrashRecord rec = {};
  ESP.rtcUserMemoryRead(CRASH_OFFSET, reinterpret_cast<uint32_t*>(&rec), sizeof(rec));
  return rec;
}

static CheckpointRecord readCheckpointRecord() {
  CheckpointRecord rec = {};
  ESP.rtcUserMemoryRead(CHECKPOINT_OFFSET, reinterpret_cast<uint32_t*>(&rec), sizeof(rec));
  return rec;
}

void setUp(void) {
  espTestClearRtc();
  espTestSetResetReason(REASON_DEFAULT_RST);
  s_logCount   = 0;
  s_lastModule = 0xFF;   // the value the firmware boots with
}

void tearDown(void) {}

// ============================================================
// MODULE TABLE — must stay in lockstep with the CP_* enum
//
// These two live in different files (Crash.h's enum, Crash.cpp's name array)
// with nothing but comment discipline holding them together. Insert an enum
// value in the middle and every post-mortem log silently names the wrong
// subsystem from then on - and those DEFERRED lines are the first thing
// consulted in a crash investigation, so a wrong one sends it off in the
// wrong direction entirely.
// ============================================================

static void test_module_table_has_an_entry_for_every_checkpoint_id(void) {
  TEST_ASSERT_EQUAL_UINT32(CP_TEN_MIN + 1, MODULE_COUNT);
}

static void test_every_module_name_matches_its_enum_ordinal(void) {
  TEST_ASSERT_EQUAL_STRING("INIT",       moduleName(CP_INIT));
  TEST_ASSERT_EQUAL_STRING("WEB",        moduleName(CP_WEB));
  TEST_ASSERT_EQUAL_STRING("BLE",        moduleName(CP_BLE));
  TEST_ASSERT_EQUAL_STRING("MDNS",       moduleName(CP_MDNS));
  TEST_ASSERT_EQUAL_STRING("MQTT",       moduleName(CP_MQTT));
  TEST_ASSERT_EQUAL_STRING("MQTT_PEND",  moduleName(CP_MQTT_PEND));
  TEST_ASSERT_EQUAL_STRING("HOOK",       moduleName(CP_HOOK));
  TEST_ASSERT_EQUAL_STRING("TEMP_REQ",   moduleName(CP_TEMP_REQ));
  TEST_ASSERT_EQUAL_STRING("TEMP_READ",  moduleName(CP_TEMP_READ));
  TEST_ASSERT_EQUAL_STRING("PROBE_SCAN", moduleName(CP_PROBE_SCAN));
  TEST_ASSERT_EQUAL_STRING("TILT",       moduleName(CP_TILT));
  TEST_ASSERT_EQUAL_STRING("FERM",       moduleName(CP_FERM));
  TEST_ASSERT_EQUAL_STRING("CLOUD",      moduleName(CP_CLOUD));
  TEST_ASSERT_EQUAL_STRING("MQTT_PUB",   moduleName(CP_MQTT_PUB));
  TEST_ASSERT_EQUAL_STRING("TEN_MIN",    moduleName(CP_TEN_MIN));
}

static void test_unknown_module_id_degrades_to_a_placeholder(void) {
  TEST_ASSERT_EQUAL_STRING("?", moduleName(MODULE_COUNT));
  TEST_ASSERT_EQUAL_STRING("?", moduleName(0xFF));   // the pre-first-call value
}

// ============================================================
// CHECKPOINT BREADCRUMB
// ============================================================

static void test_checkpoint_writes_the_module_with_its_magic(void) {
  checkpoint(CP_FERM);
  CheckpointRecord rec = readCheckpointRecord();
  TEST_ASSERT_EQUAL_HEX32(CP_MAGIC, rec.magic);
  TEST_ASSERT_EQUAL_UINT32(CP_FERM, rec.module);
}

// The very first call must always write: s_lastModule starts at 0xFF, outside
// the enum, so even checkpoint(CP_INIT) with its module value of 0 isn't
// mistaken for "unchanged".
static void test_first_checkpoint_writes_even_for_module_zero(void) {
  checkpoint(CP_INIT);
  CheckpointRecord rec = readCheckpointRecord();
  TEST_ASSERT_EQUAL_HEX32(CP_MAGIC, rec.magic);
  TEST_ASSERT_EQUAL_UINT32(CP_INIT, rec.module);
}

// The dedupe is what makes this cheap enough to call on every loop pass -
// without it the RTC would be written thousands of times a second.
static void test_repeat_checkpoint_for_the_same_module_does_not_rewrite(void) {
  checkpoint(CP_TILT);
  espTestClearRtc();          // any subsequent write would show up here
  checkpoint(CP_TILT);
  CheckpointRecord rec = readCheckpointRecord();
  TEST_ASSERT_EQUAL_HEX32(0, rec.magic);
}

static void test_checkpoint_writes_again_when_the_module_changes(void) {
  checkpoint(CP_TILT);
  espTestClearRtc();
  checkpoint(CP_MQTT);
  CheckpointRecord rec = readCheckpointRecord();
  TEST_ASSERT_EQUAL_HEX32(CP_MAGIC, rec.magic);
  TEST_ASSERT_EQUAL_UINT32(CP_MQTT, rec.module);
}

// The two records must not overlap - CHECKPOINT_OFFSET is hand-derived from
// sizeof(CrashRecord) and a static_assert pins it, but this proves it end to
// end: a checkpoint write must not corrupt a crash record already in RTC.
static void test_checkpoint_write_does_not_disturb_the_crash_record(void) {
  stageCrashRecord(CP_WEB);
  checkpoint(CP_FERM);
  CrashRecord rec = readCrashRecord();
  TEST_ASSERT_EQUAL_HEX32(CRASH_MAGIC, rec.magic);
  TEST_ASSERT_EQUAL_HEX32(0x40201234, rec.epc1);
}

// ============================================================
// THE UNEXPECTED-RESET GATE
//
// Clean reboots must stay silent or every OTA update and every /reboot fills
// the log with crash-shaped noise.
// ============================================================

static void test_power_on_reset_logs_nothing(void) {
  espTestSetResetReason(REASON_DEFAULT_RST);
  checkpoint(CP_FERM);
  crashLogPendingDeferred();
  TEST_ASSERT_EQUAL_INT(0, s_logCount);
}

static void test_external_reset_logs_nothing(void) {
  espTestSetResetReason(REASON_EXT_SYS_RST);
  checkpoint(CP_FERM);
  crashLogPendingDeferred();
  TEST_ASSERT_EQUAL_INT(0, s_logCount);
}

static void test_deep_sleep_wake_logs_nothing(void) {
  espTestSetResetReason(REASON_DEEP_SLEEP_AWAKE);
  checkpoint(CP_FERM);
  crashLogPendingDeferred();
  TEST_ASSERT_EQUAL_INT(0, s_logCount);
}

static void test_hardware_watchdog_reset_is_reported(void) {
  espTestSetResetReason(REASON_WDT_RST);
  checkpoint(CP_FERM);
  crashLogPendingDeferred();
  TEST_ASSERT_GREATER_THAN_INT(0, s_logCount);
  TEST_ASSERT_TRUE(logContains("Hardware Watchdog"));
  TEST_ASSERT_TRUE(logContains("FERM"));
}

static void test_soft_watchdog_reset_is_reported(void) {
  espTestSetResetReason(REASON_SOFT_WDT_RST);
  checkpoint(CP_MQTT);
  crashLogPendingDeferred();
  TEST_ASSERT_TRUE(logContains("Software Watchdog"));
  TEST_ASSERT_TRUE(logContains("MQTT"));
}

// Soft restart is deliberately treated as unexpected: the SDK routes
// panic()/assert() failures through it, so they arrive looking exactly like a
// clean ESP.restart(). Excluding it would hide real faults.
static void test_soft_restart_is_reported_because_panic_uses_that_path(void) {
  espTestSetResetReason(REASON_SOFT_RESTART);
  checkpoint(CP_WEB);
  crashLogPendingDeferred();
  TEST_ASSERT_TRUE(logContains("Software/System restart"));
  TEST_ASSERT_TRUE(logContains("WEB"));
}

static void test_exception_reset_is_reported(void) {
  espTestSetResetReason(REASON_EXCEPTION_RST);
  checkpoint(CP_TILT);
  crashLogPendingDeferred();
  TEST_ASSERT_GREATER_THAN_INT(0, s_logCount);
}

// ============================================================
// REPORTING: CRASH RECORD, CHECKPOINT-ONLY, AND NEITHER
// ============================================================

static void test_crash_record_reports_the_full_register_frame(void) {
  espTestSetResetReason(REASON_EXCEPTION_RST);
  stageCrashRecord(CP_CLOUD);
  crashLogPendingDeferred();
  TEST_ASSERT_TRUE(logContains("Crash detail"));
  TEST_ASSERT_TRUE(logContains("0x40205678"));   // EPC2
  TEST_ASSERT_TRUE(logContains("0x40209abc"));   // EPC3
  TEST_ASSERT_TRUE(logContains("0x4020def0"));   // DEPC
  TEST_ASSERT_TRUE(logContains("last=CLOUD"));
}

// 24 stack words at 8 per line = 3 STACK lines, after the detail and SP header.
static void test_crash_record_dumps_the_whole_stack_slice(void) {
  espTestSetResetReason(REASON_EXCEPTION_RST);
  stageCrashRecord(CP_FERM);
  crashLogPendingDeferred();
  TEST_ASSERT_TRUE(logContains("STACK 00"));
  TEST_ASSERT_TRUE(logContains("STACK 08"));
  TEST_ASSERT_TRUE(logContains("STACK 16"));
  TEST_ASSERT_EQUAL_INT(5, s_logCount);
}

// The magic is cleared once reported, so the same crash isn't re-logged on
// every subsequent boot - RTC survives a reset, and without this the record
// would look fresh forever.
static void test_reported_crash_is_cleared_so_it_is_not_logged_twice(void) {
  espTestSetResetReason(REASON_EXCEPTION_RST);
  stageCrashRecord(CP_FERM);
  crashLogPendingDeferred();
  TEST_ASSERT_TRUE(logContains("Crash detail"));

  s_logCount = 0;
  crashLogPendingDeferred();
  TEST_ASSERT_FALSE(logContains("Crash detail"));
}

static void test_stale_magic_falls_through_to_the_checkpoint_path(void) {
  espTestSetResetReason(REASON_WDT_RST);
  stageCrashRecord(CP_CLOUD, 0xDEADBEEF);   // wrong magic = no usable record
  checkpoint(CP_PROBE_SCAN);
  crashLogPendingDeferred();
  TEST_ASSERT_FALSE(logContains("Crash detail"));
  TEST_ASSERT_TRUE(logContains("last subsystem = PROBE_SCAN"));
}

// The hardware-watchdog case: the exception callback never ran, so the
// breadcrumb is the only evidence there is.
static void test_checkpoint_only_path_names_the_subsystem_and_its_id(void) {
  espTestSetResetReason(REASON_WDT_RST);
  checkpoint(CP_MDNS);
  crashLogPendingDeferred();
  TEST_ASSERT_TRUE(logContains("last subsystem = MDNS (3)"));
}

static void test_no_records_at_all_still_reports_the_reset_reason(void) {
  espTestSetResetReason(REASON_WDT_RST);
  crashLogPendingDeferred();
  TEST_ASSERT_TRUE(logContains("Hardware Watchdog"));
  TEST_ASSERT_TRUE(logContains("no checkpoint recorded"));
}

// ============================================================
// custom_crash_callback — runs in exception context
// ============================================================

// A stand-in for the crashing task's stack.
//
// custom_crash_callback takes the stack pointer as a uint32_t, which is exact
// on the ESP8266 - every address there fits in 32 bits. The host is 64-bit, so
// an ordinary static buffer (address ~0x00007FF6_xxxxxxxx) cannot survive that
// round trip: truncating it to 32 bits and dereferencing the result reads a
// wild address and faults. The copy loop is therefore only reachable natively
// from memory reserved below the 4 GB line.
//
// The base is the ESP8266's own DRAM stack range, so addresses printed in a
// failure message look like the ones a real crash dump carries.
#define FAKE_STACK_BASE  0x3FFF0000u
#define FAKE_STACK_WORDS 32

static uint32_t* s_fakeStack = nullptr;

static bool reserveFakeStack() {
  if (s_fakeStack) return true;
#ifdef _WIN32
  void* p = VirtualAlloc((LPVOID)(uintptr_t)FAKE_STACK_BASE, 4096,
                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
  void* p = mmap((void*)(uintptr_t)FAKE_STACK_BASE, 4096,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) p = nullptr;
#endif
  // On POSIX the address is only a hint, so confirm it really landed low -
  // anywhere above 4 GB and the truncation problem is still present.
  if (p && (uintptr_t)p <= 0xFFFFFFFFu) {
    s_fakeStack = (uint32_t*)p;
    return true;
  }
  return false;
}

// Skip rather than fail if the reservation is refused: an occupied address
// range is an environment problem, not a defect in the code under test.
#define REQUIRE_FAKE_STACK() \
  if (!reserveFakeStack()) TEST_IGNORE_MESSAGE("no free memory below 4GB for a fake stack")

static void fillFakeStack() {
  for (int i = 0; i < FAKE_STACK_WORDS; i++) s_fakeStack[i] = 0xAA000000u + i;
}

static uint32_t fakeStackAddr(int wordOffset) {
  return (uint32_t)(uintptr_t)(s_fakeStack + wordOffset);
}

static rst_info makeInfo() {
  rst_info info = {};
  info.reason   = REASON_EXCEPTION_RST;
  info.exccause = 9;
  info.epc1     = 0x40201111;
  info.epc2     = 0x40202222;
  info.epc3     = 0x40203333;
  info.excvaddr = 0x3FFF0000;
  info.depc     = 0x40204444;
  return info;
}

static void test_callback_captures_the_register_frame_and_checkpoint(void) {
  REQUIRE_FAKE_STACK();
  fillFakeStack();
  checkpoint(CP_TEMP_READ);
  rst_info info = makeInfo();
  custom_crash_callback(&info, fakeStackAddr(0), fakeStackAddr(FAKE_STACK_WORDS));

  CrashRecord rec = readCrashRecord();
  TEST_ASSERT_EQUAL_HEX32(CRASH_MAGIC, rec.magic);
  TEST_ASSERT_EQUAL_UINT32(9, rec.exccause);
  TEST_ASSERT_EQUAL_HEX32(0x40201111, rec.epc1);
  TEST_ASSERT_EQUAL_HEX32(0x40204444, rec.depc);
  TEST_ASSERT_EQUAL_UINT32(CP_TEMP_READ, rec.lastCheckpoint);
}

static void test_callback_captures_the_stack_slice(void) {
  REQUIRE_FAKE_STACK();
  fillFakeStack();
  rst_info info = makeInfo();
  custom_crash_callback(&info, fakeStackAddr(0), fakeStackAddr(FAKE_STACK_WORDS));

  CrashRecord rec = readCrashRecord();
  TEST_ASSERT_EQUAL_HEX32(0xAA000000, rec.stack[0]);
  TEST_ASSERT_EQUAL_HEX32(0xAA000017, rec.stack[STACK_WORDS - 1]);
}

// A misaligned SP would fault on the very first load - an Exception 28 raised
// from inside the exception handler, which loses the diagnostic completely.
// The frame must still be recorded; only the stack slice is skipped.
static void test_misaligned_stack_pointer_skips_the_slice_but_keeps_the_frame(void) {
  REQUIRE_FAKE_STACK();
  fillFakeStack();
  rst_info info = makeInfo();
  custom_crash_callback(&info, fakeStackAddr(0) + 1, fakeStackAddr(FAKE_STACK_WORDS));

  CrashRecord rec = readCrashRecord();
  TEST_ASSERT_EQUAL_HEX32(CRASH_MAGIC, rec.magic);
  TEST_ASSERT_EQUAL_HEX32(0x40201111, rec.epc1);
  TEST_ASSERT_EQUAL_HEX32(0, rec.stack[0]);   // nothing copied
}

static void test_inverted_stack_bounds_skip_the_slice(void) {
  REQUIRE_FAKE_STACK();
  fillFakeStack();
  rst_info info = makeInfo();
  custom_crash_callback(&info, fakeStackAddr(FAKE_STACK_WORDS), fakeStackAddr(0));

  CrashRecord rec = readCrashRecord();
  TEST_ASSERT_EQUAL_HEX32(CRASH_MAGIC, rec.magic);
  TEST_ASSERT_EQUAL_HEX32(0, rec.stack[0]);
}

// Near the top of the stack there may be fewer than STACK_WORDS words left;
// copying the full 24 would read past stack_end into whatever follows.
static void test_short_stack_is_clamped_to_what_is_available(void) {
  REQUIRE_FAKE_STACK();
  fillFakeStack();
  rst_info info = makeInfo();
  custom_crash_callback(&info, fakeStackAddr(0), fakeStackAddr(4));   // only 4 words

  CrashRecord rec = readCrashRecord();
  TEST_ASSERT_EQUAL_HEX32(0xAA000000, rec.stack[0]);
  TEST_ASSERT_EQUAL_HEX32(0xAA000003, rec.stack[3]);
  TEST_ASSERT_EQUAL_HEX32(0, rec.stack[4]);   // untouched, not over-read
}

// End to end: what the callback writes is what the next boot reports.
static void test_callback_output_is_readable_by_the_reporting_path(void) {
  REQUIRE_FAKE_STACK();
  fillFakeStack();
  checkpoint(CP_MQTT_PUB);
  rst_info info = makeInfo();
  custom_crash_callback(&info, fakeStackAddr(0), fakeStackAddr(FAKE_STACK_WORDS));

  espTestSetResetReason(REASON_EXCEPTION_RST);
  s_logCount = 0;
  crashLogPendingDeferred();
  TEST_ASSERT_TRUE(logContains("last=MQTT_PUB"));
  TEST_ASSERT_TRUE(logContains("0x40202222"));   // EPC2 from the callback
}

// ============================================================

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_module_table_has_an_entry_for_every_checkpoint_id);
  RUN_TEST(test_every_module_name_matches_its_enum_ordinal);
  RUN_TEST(test_unknown_module_id_degrades_to_a_placeholder);

  RUN_TEST(test_checkpoint_writes_the_module_with_its_magic);
  RUN_TEST(test_first_checkpoint_writes_even_for_module_zero);
  RUN_TEST(test_repeat_checkpoint_for_the_same_module_does_not_rewrite);
  RUN_TEST(test_checkpoint_writes_again_when_the_module_changes);
  RUN_TEST(test_checkpoint_write_does_not_disturb_the_crash_record);

  RUN_TEST(test_power_on_reset_logs_nothing);
  RUN_TEST(test_external_reset_logs_nothing);
  RUN_TEST(test_deep_sleep_wake_logs_nothing);
  RUN_TEST(test_hardware_watchdog_reset_is_reported);
  RUN_TEST(test_soft_watchdog_reset_is_reported);
  RUN_TEST(test_soft_restart_is_reported_because_panic_uses_that_path);
  RUN_TEST(test_exception_reset_is_reported);

  RUN_TEST(test_crash_record_reports_the_full_register_frame);
  RUN_TEST(test_crash_record_dumps_the_whole_stack_slice);
  RUN_TEST(test_reported_crash_is_cleared_so_it_is_not_logged_twice);
  RUN_TEST(test_stale_magic_falls_through_to_the_checkpoint_path);
  RUN_TEST(test_checkpoint_only_path_names_the_subsystem_and_its_id);
  RUN_TEST(test_no_records_at_all_still_reports_the_reset_reason);

  RUN_TEST(test_callback_captures_the_register_frame_and_checkpoint);
  RUN_TEST(test_callback_captures_the_stack_slice);
  RUN_TEST(test_misaligned_stack_pointer_skips_the_slice_but_keeps_the_frame);
  RUN_TEST(test_inverted_stack_bounds_skip_the_slice);
  RUN_TEST(test_short_stack_is_clamped_to_what_is_available);
  RUN_TEST(test_callback_output_is_readable_by_the_reporting_path);

  return UNITY_END();
}
