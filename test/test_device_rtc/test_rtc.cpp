// On-device (hardware) tests for the post-mortem machinery in
// OurBrewbot/Crash.cpp, run on the real ESP8266 over serial.
//
// `pio test -e nodemcuv2_test --upload-port COM6` REPLACES the running
// firmware for the duration of the run. Only do this when the fermenter
// isn't mid-batch, and never without explicit go-ahead.
//
// WHY THIS CANNOT BE A NATIVE TEST
// test_native_crash covers the same logic, but against
// test/stubs/Esp.h's `static uint8_t g_espRtcMem[512]` - a plain array in
// one host process. That harness cannot answer the question the whole
// design rests on: does anything actually survive a reset? Nor can it
// confirm that dword offset 35 is really free, that the SDK doesn't use
// that region for itself, or that a write past the bank is rejected rather
// than wrapping. Each of those failing would silently produce an empty
// post-mortem exactly when one was needed - the diagnostic is only ever
// read after a crash has already happened.
//
// HOW THE TWO-BOOT PATTERN WORKS
// A marker in an RTC slot above both production records tells the two boots
// apart. First boot writes the records and calls ESP.restart() WITHOUT
// UNITY_BEGIN/END, so the runner sees no test output and simply keeps
// waiting; second boot finds the marker and runs the assertions. The
// NodeMCU's USB-serial bridge is a separate chip that stays powered through
// a reset, so the port survives the reboot. The marker is cleared after
// UNITY_END so a repeat run starts from the write phase again rather than
// asserting against stale records.

#include <Arduino.h>
#include <unity.h>

#include "../../OurBrewbot/Crash.h"
#include "../../OurBrewbot/Log.h"

// ---- log capture: the DEFERRED lines ARE the observable behaviour ----
#define MAX_LOG_LINES 16
#define MAX_LOG_LEN   200
static char s_logLines[MAX_LOG_LINES][MAX_LOG_LEN];
static int  s_logCount = 0;

void logMsgImpl(uint8_t, PGM_P fmt, ...) {
  if (s_logCount >= MAX_LOG_LINES) return;
  va_list args;
  va_start(args, fmt);
  vsnprintf_P(s_logLines[s_logCount], MAX_LOG_LEN, fmt, args);
  va_end(args);
  s_logCount++;
}

static bool logContains(const char* needle) {
  for (int i = 0; i < s_logCount; i++) {
    if (strstr(s_logLines[i], needle)) return true;
  }
  return false;
}

// Crash.cpp is #included directly (not linked) so the real production source
// is what runs, and so its anonymous namespace - CrashRecord, the magics and
// the offsets - is in scope to be read back raw.
#include "../../OurBrewbot/Crash.cpp"

extern "C" void custom_crash_callback(struct rst_info* info,
                                      uint32_t stack, uint32_t stack_end);

// ---- two-boot marker, in a slot above CrashRecord (0-34) and
// CheckpointRecord (35-36) ----
static const uint32_t MARKER_OFFSET = 40;
static const uint32_t MARKER_MAGIC  = 0x7E577E57u;

// The subsystem and register values written before the reset, asserted after.
static const uint8_t  EXPECT_MODULE   = CP_FERM;
static const uint32_t EXPECT_EXCCAUSE = 28;
static const uint32_t EXPECT_EPC1     = 0x4020ABCDu;
static const uint32_t EXPECT_EPC2     = 0x00000002u;
static const uint32_t EXPECT_EPC3     = 0x00000003u;
static const uint32_t EXPECT_DEPC     = 0x00000004u;
static const uint32_t EXPECT_EXCVADDR = 0x3FFF1234u;
static const uint32_t STACK_PATTERN   = 0xA5A50000u;

// Backing store for the fake stack slice handed to custom_crash_callback.
// Static so it is 4-byte aligned and lives outside the callback's frame.
static uint32_t s_fakeStack[STACK_WORDS];

static bool markerPresent() {
  uint32_t marker = 0;
  if (!ESP.rtcUserMemoryRead(MARKER_OFFSET, &marker, sizeof(marker))) return false;
  return marker == MARKER_MAGIC;
}

static void writeMarker(uint32_t value) {
  ESP.rtcUserMemoryWrite(MARKER_OFFSET, &value, sizeof(value));
}

// Everything the assertions below expect to find after the reset.
static void writeRecordsAndRestart() {
  checkpoint(EXPECT_MODULE);

  for (size_t i = 0; i < STACK_WORDS; i++) s_fakeStack[i] = STACK_PATTERN | (uint32_t)i;

  struct rst_info info;
  memset(&info, 0, sizeof(info));
  info.reason   = REASON_EXCEPTION_RST;
  info.exccause = EXPECT_EXCCAUSE;
  info.epc1     = EXPECT_EPC1;
  info.epc2     = EXPECT_EPC2;
  info.epc3     = EXPECT_EPC3;
  info.depc     = EXPECT_DEPC;
  info.excvaddr = EXPECT_EXCVADDR;

  custom_crash_callback(&info,
                        (uint32_t)(uintptr_t)s_fakeStack,
                        (uint32_t)(uintptr_t)s_fakeStack + sizeof(s_fakeStack));

  writeMarker(MARKER_MAGIC);
  delay(500);        // let the writes settle before pulling the rug
  ESP.restart();
}

// ---- reading the records back raw ----

static bool readCrashRecord(CrashRecord& rec) {
  return ESP.rtcUserMemoryRead(0, reinterpret_cast<uint32_t*>(&rec), sizeof(rec));
}

static bool readCheckpointRecord(CheckpointRecord& rec) {
  return ESP.rtcUserMemoryRead(CHECKPOINT_OFFSET,
                               reinterpret_cast<uint32_t*>(&rec), sizeof(rec));
}

void setUp(void) { s_logCount = 0; }
void tearDown(void) {}

// ============================================================
// The tests below run in RUN_TEST order and are deliberately ordered:
// the crash record has to be asserted before crashLogPendingDeferred()
// consumes it, and the checkpoint-only path is only reachable afterwards.
// ============================================================

// Guards every other assertion in this file: if we didn't actually come back
// from a restart, "survived" would mean nothing.
void test_we_arrived_here_via_a_restart(void) {
  TEST_ASSERT_EQUAL_STRING("Software/System restart", ESP.getResetReason().c_str());
}

void test_checkpoint_record_survives_a_reset(void) {
  CheckpointRecord rec;
  TEST_ASSERT_TRUE(readCheckpointRecord(rec));
  TEST_ASSERT_EQUAL_HEX32(CP_MAGIC, rec.magic);
  TEST_ASSERT_EQUAL_UINT32(EXPECT_MODULE, rec.module);
}

void test_crash_record_survives_a_reset(void) {
  CrashRecord rec;
  TEST_ASSERT_TRUE(readCrashRecord(rec));
  TEST_ASSERT_EQUAL_HEX32(CRASH_MAGIC, rec.magic);
  TEST_ASSERT_EQUAL_HEX32(EXPECT_EPC1, rec.epc1);
  TEST_ASSERT_EQUAL_HEX32(EXPECT_EPC2, rec.epc2);
  TEST_ASSERT_EQUAL_HEX32(EXPECT_EPC3, rec.epc3);
  TEST_ASSERT_EQUAL_HEX32(EXPECT_DEPC, rec.depc);
  TEST_ASSERT_EQUAL_HEX32(EXPECT_EXCVADDR, rec.excvaddr);
  TEST_ASSERT_EQUAL_UINT32(EXPECT_EXCCAUSE, rec.exccause);
}

// The register frame is only half the value - the stack slice is what
// addr2line turns into a call chain.
void test_captured_stack_slice_survives_a_reset(void) {
  CrashRecord rec;
  TEST_ASSERT_TRUE(readCrashRecord(rec));
  for (size_t i = 0; i < STACK_WORDS; i++) {
    TEST_ASSERT_EQUAL_HEX32(STACK_PATTERN | (uint32_t)i, rec.stack[i]);
  }
}

// The crash record carries the breadcrumb too, so an exception dump names
// its subsystem without needing the separate checkpoint slot.
void test_crash_record_carries_the_last_checkpoint(void) {
  CrashRecord rec;
  TEST_ASSERT_TRUE(readCrashRecord(rec));
  TEST_ASSERT_EQUAL_UINT32(EXPECT_MODULE, rec.lastCheckpoint);
}

// The two records must not overlap: CHECKPOINT_OFFSET is hand-computed as
// the dword after CrashRecord, and a static_assert pins the size, but only
// hardware proves the arithmetic against a real 512-byte bank.
void test_the_two_records_do_not_overlap(void) {
  CrashRecord crash;
  CheckpointRecord cp;
  TEST_ASSERT_TRUE(readCrashRecord(crash));
  TEST_ASSERT_TRUE(readCheckpointRecord(cp));
  TEST_ASSERT_EQUAL_HEX32(CRASH_MAGIC, crash.magic);
  TEST_ASSERT_EQUAL_HEX32(CP_MAGIC, cp.magic);
  TEST_ASSERT_EQUAL_UINT32(35u * 4u, sizeof(CrashRecord));
}

// Consumes the crash record - everything above must have run already.
void test_deferred_report_names_the_registers_stack_and_subsystem(void) {
  crashLogPendingDeferred();
  TEST_ASSERT_TRUE(logContains("Crash detail:"));
  TEST_ASSERT_TRUE(logContains("last=FERM"));
  TEST_ASSERT_TRUE(logContains("Stack: SP="));
  TEST_ASSERT_TRUE(logContains("STACK 00:"));
}

// The record is cleared once reported, so a later boot doesn't re-log a
// crash that already happened.
void test_the_crash_record_is_cleared_once_reported(void) {
  CrashRecord rec;
  TEST_ASSERT_TRUE(readCrashRecord(rec));
  TEST_ASSERT_NOT_EQUAL(CRASH_MAGIC, rec.magic);
}

// With no crash record left, the same call falls to the checkpoint-only
// path - the hardware-watchdog case, where the exception handler never ran.
void test_checkpoint_only_path_reports_the_subsystem(void) {
  crashLogPendingDeferred();
  TEST_ASSERT_TRUE(logContains("Software/System restart: last subsystem = FERM (11)"));
  TEST_ASSERT_FALSE(logContains("Crash detail:"));
}

// checkpoint() skips the RTC write when the module hasn't changed. Verified
// here against real RTC rather than an array, since the dedupe is what keeps
// this off the hot path in loop().
void test_checkpoint_updates_the_record_on_a_new_module(void) {
  checkpoint(CP_TILT);
  CheckpointRecord rec;
  TEST_ASSERT_TRUE(readCheckpointRecord(rec));
  TEST_ASSERT_EQUAL_UINT32(CP_TILT, rec.module);
}

// The SDK bank is 512 bytes; test/stubs/Esp.h assumes an out-of-range access
// is REJECTED rather than clamped or wrapped. If the real API wrapped
// instead, a bad offset would quietly corrupt another record.
void test_a_write_past_the_bank_is_rejected(void) {
  uint32_t scratch[2] = {0xDEADBEEF, 0xDEADBEEF};
  TEST_ASSERT_FALSE(ESP.rtcUserMemoryWrite(127, scratch, sizeof(scratch)));
  TEST_ASSERT_FALSE(ESP.rtcUserMemoryRead(127, scratch, sizeof(scratch)));
}

void setup() {
  delay(2000);  // let the board settle after upload/reset before anything else

  if (!markerPresent()) {
    writeRecordsAndRestart();   // does not return
  }

  UNITY_BEGIN();

  RUN_TEST(test_we_arrived_here_via_a_restart);
  RUN_TEST(test_checkpoint_record_survives_a_reset);
  RUN_TEST(test_crash_record_survives_a_reset);
  RUN_TEST(test_captured_stack_slice_survives_a_reset);
  RUN_TEST(test_crash_record_carries_the_last_checkpoint);
  RUN_TEST(test_the_two_records_do_not_overlap);
  RUN_TEST(test_deferred_report_names_the_registers_stack_and_subsystem);
  RUN_TEST(test_the_crash_record_is_cleared_once_reported);
  RUN_TEST(test_checkpoint_only_path_reports_the_subsystem);
  RUN_TEST(test_checkpoint_updates_the_record_on_a_new_module);
  RUN_TEST(test_a_write_past_the_bank_is_rejected);

  UNITY_END();

  // Leave RTC as a cold boot would, so a repeat run starts from the write
  // phase instead of asserting against records this run already consumed.
  writeMarker(0);
}

void loop() {}
