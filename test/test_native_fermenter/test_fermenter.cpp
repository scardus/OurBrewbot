// Native (host) tests for the heating/cooling hysteresis state machine
// (processSingleFermenter) and the alarm dwell-timer logic
// (checkFermenterAlarm) in OurBrewbot/Fermenter.cpp.
//
// Fermenter.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test - see test/stubs/ for the minimal
// Arduino-core stand-ins, and below for the storage/test-doubles the rest
// of the firmware would normally provide.

#include <unity.h>
#include <cstdint>

#include "../../OurBrewbot/Config.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
FermenterConfig g_fermenters[MAX_FERMENTERS];
SmartPlugConfig  g_smartPlugs[MAX_SMART_PLUGS];
TiltConfig       g_tilts[MAX_TILTS];
iSpindelConfig   g_iSpindels[MAX_ISPINDELS];
GlobalConfig     g_globalConfig;
bool g_fermenterDebugMode = false;
FermenterDebugOverride g_fermenterDebugOverrides[MAX_FERMENTERS];

// ---- millis(), settable per test ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op stubs ----
bool saveFermenterConfig() { return true; }
void logMsgImpl(uint8_t, PGM_P, ...) {}
// processProfiles() is only referenced inside processFermenters() (never
// called by these tests), but the whole file compiles as one TU so its
// symbol still needs to resolve at link time.
void processProfiles() {}

// ---- sensor test double, settable per test ----
static float s_controlTemp;
float getControlTemp(uint8_t) { return s_controlTemp; }

// ---- smart plug test double: records every switch call ----
struct PlugCall { uint8_t plugIndex; bool on; };
static PlugCall s_plugCalls[32];
static int      s_plugCallCount;
void smartPlugSwitch(uint8_t plugIndex, bool on) {
  if (s_plugCallCount < 32) s_plugCalls[s_plugCallCount++] = {plugIndex, on};
}
// Last recorded on/off state for a given plug, or -1 if never called.
static int lastPlugState(uint8_t plugIndex) {
  for (int i = s_plugCallCount - 1; i >= 0; i--) {
    if (s_plugCalls[i].plugIndex == plugIndex) return s_plugCalls[i].on ? 1 : 0;
  }
  return -1;
}

// The functions under test, plus everything else in Fermenter.cpp.
// This also brings the file-static s_state[]/s_lastCoolingStop[]/
// s_outOfRangeSince[] arrays into scope below, for state reset between tests.
#include "../../OurBrewbot/Fermenter.cpp"

// ---- test fixture ----

static const uint8_t F = 0;  // fermenter index used by most tests

void setUp(void) {
  for (int i = 0; i < MAX_FERMENTERS; i++) g_fermenters[i] = FermenterConfig{};
  g_fermenters[F].power       = true;
  g_fermenters[F].tempControl = true;
  for (int i = 0; i < MAX_SMART_PLUGS; i++) g_smartPlugs[i] = SmartPlugConfig{};
  // The gravity chain reads these three, so they need clearing between tests
  // the same way the plug/fermenter arrays do.
  for (int i = 0; i < MAX_TILTS; i++)     g_tilts[i]     = TiltConfig{};
  for (int i = 0; i < MAX_ISPINDELS; i++) g_iSpindels[i] = iSpindelConfig{};
  g_fermenterDebugMode = false;
  for (int i = 0; i < MAX_FERMENTERS; i++) g_fermenterDebugOverrides[i] = FermenterDebugOverride{};
  s_controlTemp   = TEMP_NONE;
  s_millis        = 0;
  s_plugCallCount = 0;
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    s_state[i]            = STATUS_IDLE;
    s_lastCoolingStop[i]  = 0;
    s_outOfRangeSince[i]  = 0;
  }
  g_globalConfig = GlobalConfig{};
}

void tearDown(void) {}

// ---- processSingleFermenter: heating/cooling state machine ----

void test_idle_to_heating_when_below_floor(void) {
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;
  s_controlTemp = 17.0f;
  processSingleFermenter(F);
  TEST_ASSERT_EQUAL(STATUS_HEATING, g_fermenters[F].status);
}

void test_idle_to_cooling_when_above_ceiling_and_delay_elapsed(void) {
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;
  g_fermenters[F].compressorDelay = 10;  // minutes
  // s_lastCoolingStop defaults to 0 ("never cooled"), so start the clock
  // comfortably past the delay rather than at 0 - millis()==0 would collide
  // with that sentinel and read as "delay already elapsed" for the wrong
  // reason (a boot-time-only coincidence, not what this test means to check).
  test_setMillis(1000000);
  s_controlTemp = 23.0f;
  processSingleFermenter(F);
  TEST_ASSERT_EQUAL(STATUS_COOLING, g_fermenters[F].status);
}

void test_idle_blocks_cooling_during_compressor_delay(void) {
  g_fermenters[F].floorTemp       = 18.0f;
  g_fermenters[F].ceilingTemp     = 22.0f;
  g_fermenters[F].hysteresis      = 0.5f;
  g_fermenters[F].compressorDelay = 10;  // 10 min = 600000 ms

  // Get into COOLING, then back to IDLE, recording s_lastCoolingStop.
  // Start well after 0 (see note above) and only ever move forward.
  uint32_t t = 1000000;
  test_setMillis(t);
  s_controlTemp = 23.0f;
  processSingleFermenter(F);
  TEST_ASSERT_EQUAL(STATUS_COOLING, g_fermenters[F].status);
  s_controlTemp = 21.0f;  // <= ceiling - hysteresis
  processSingleFermenter(F);
  TEST_ASSERT_EQUAL(STATUS_IDLE, g_fermenters[F].status);

  // Immediately wants to cool again, but delay hasn't elapsed.
  test_setMillis(t + 100);  // well under 600000 ms later
  s_controlTemp = 23.0f;
  processSingleFermenter(F);
  TEST_ASSERT_EQUAL(STATUS_IDLE, g_fermenters[F].status);

  // Delay has now elapsed - cooling is allowed again.
  test_setMillis(t + 600001);
  processSingleFermenter(F);
  TEST_ASSERT_EQUAL(STATUS_COOLING, g_fermenters[F].status);
}

void test_heating_to_idle_at_floor_plus_hysteresis(void) {
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;
  g_fermenters[F].hysteresis  = 0.5f;
  s_state[F] = STATUS_HEATING;

  s_controlTemp = 18.3f;  // below floor + hysteresis
  processSingleFermenter(F);
  TEST_ASSERT_EQUAL(STATUS_HEATING, g_fermenters[F].status);

  s_controlTemp = 18.6f;  // at/above floor + hysteresis
  processSingleFermenter(F);
  TEST_ASSERT_EQUAL(STATUS_IDLE, g_fermenters[F].status);
}

void test_cooling_to_idle_at_ceiling_minus_hysteresis(void) {
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;
  g_fermenters[F].hysteresis  = 0.5f;
  s_state[F] = STATUS_COOLING;

  s_controlTemp = 21.8f;  // above ceiling - hysteresis
  processSingleFermenter(F);
  TEST_ASSERT_EQUAL(STATUS_COOLING, g_fermenters[F].status);

  s_controlTemp = 21.4f;  // at/below ceiling - hysteresis
  processSingleFermenter(F);
  TEST_ASSERT_EQUAL(STATUS_IDLE, g_fermenters[F].status);
}

void test_no_reading_forces_plugs_off_and_idle(void) {
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;
  s_state[F] = STATUS_HEATING;
  s_controlTemp = TEMP_NONE;  // disconnected probe
  processSingleFermenter(F);
  TEST_ASSERT_EQUAL(STATUS_IDLE, g_fermenters[F].status);
}

void test_plug_mapping_is_per_fermenter(void) {
  // Fermenter 1's hot/cold plugs must never be the ones fermenter 0 toggles.
  g_smartPlugs[0].fermenter = 0;
  g_smartPlugs[0].function  = PLUG_FN_F1_HOT;
  g_smartPlugs[1].fermenter = 0;
  g_smartPlugs[1].function  = PLUG_FN_F1_COLD;
  g_smartPlugs[2].fermenter = 1;
  g_smartPlugs[2].function  = PLUG_FN_F2_HOT;
  g_smartPlugs[3].fermenter = 1;
  g_smartPlugs[3].function  = PLUG_FN_F2_COLD;

  g_fermenters[1] = FermenterConfig{};
  g_fermenters[1].power       = true;
  g_fermenters[1].tempControl = true;
  g_fermenters[1].floorTemp   = 18.0f;
  g_fermenters[1].ceilingTemp = 22.0f;

  s_controlTemp = 17.0f;  // below floor -> heating, for fermenter 1 only
  processSingleFermenter(1);

  TEST_ASSERT_EQUAL(1, lastPlugState(2));   // fermenter 1's hot plug on
  TEST_ASSERT_EQUAL(-1, lastPlugState(0));  // fermenter 0's hot plug untouched
  TEST_ASSERT_EQUAL(-1, lastPlugState(1));  // fermenter 0's cold plug untouched
}

// ---- checkFermenterAlarm: severe vs. mild-with-dwell ----

void test_alarm_skipped_when_powered_off(void) {
  g_fermenters[F].power = false;
  g_fermenters[F].alarm = true;  // pre-existing value must be left alone
  checkFermenterAlarm(F);
  TEST_ASSERT_TRUE(g_fermenters[F].alarm);
}

void test_alarm_skipped_on_no_reading(void) {
  s_controlTemp = TEMP_NONE;
  g_fermenters[F].alarm = true;
  checkFermenterAlarm(F);
  TEST_ASSERT_TRUE(g_fermenters[F].alarm);
}

void test_alarm_clears_when_in_band(void) {
  g_fermenters[F].floorTemp      = 18.0f;
  g_fermenters[F].ceilingTemp    = 22.0f;
  g_fermenters[F].alarmTolerance = 3.0f;
  g_fermenters[F].alarm          = true;
  s_controlTemp = 20.0f;
  checkFermenterAlarm(F);
  TEST_ASSERT_FALSE(g_fermenters[F].alarm);
}

void test_alarm_fires_immediately_on_severe_deviation(void) {
  g_fermenters[F].floorTemp      = 18.0f;
  g_fermenters[F].ceilingTemp    = 22.0f;
  g_fermenters[F].alarmTolerance = 3.0f;
  s_controlTemp = 26.0f;  // 4.0 over ceiling >= 3.0 tolerance
  checkFermenterAlarm(F);
  TEST_ASSERT_TRUE(g_fermenters[F].alarm);
}

void test_alarm_does_not_fire_immediately_on_mild_deviation(void) {
  g_fermenters[F].floorTemp      = 18.0f;
  g_fermenters[F].ceilingTemp    = 22.0f;
  g_fermenters[F].alarmTolerance = 3.0f;
  g_globalConfig.alarmDwellSec   = 60;
  s_controlTemp = 23.0f;  // 1.0 over ceiling, < 3.0 tolerance
  checkFermenterAlarm(F);
  TEST_ASSERT_FALSE(g_fermenters[F].alarm);
}

void test_alarm_fires_after_dwell_elapses(void) {
  g_fermenters[F].floorTemp      = 18.0f;
  g_fermenters[F].ceilingTemp    = 22.0f;
  g_fermenters[F].alarmTolerance = 3.0f;
  g_globalConfig.alarmDwellSec   = 60;
  s_controlTemp = 23.0f;  // mild deviation, starts the dwell timer

  // s_outOfRangeSince defaults to 0 ("not out of range"), so start the
  // clock comfortably past 0 - millis()==0 would collide with that
  // sentinel and make the dwell-start bookkeeping below re-trigger every
  // call instead of measuring real elapsed time.
  uint32_t t = 1000000;
  test_setMillis(t);
  checkFermenterAlarm(F);
  TEST_ASSERT_FALSE(g_fermenters[F].alarm);

  test_setMillis(t + 59000);  // just under 60 s
  checkFermenterAlarm(F);
  TEST_ASSERT_FALSE(g_fermenters[F].alarm);

  test_setMillis(t + 60001);  // dwell elapsed
  checkFermenterAlarm(F);
  TEST_ASSERT_TRUE(g_fermenters[F].alarm);
}

void test_alarm_dwell_resets_on_return_to_band(void) {
  g_fermenters[F].floorTemp      = 18.0f;
  g_fermenters[F].ceilingTemp    = 22.0f;
  g_fermenters[F].alarmTolerance = 3.0f;
  g_globalConfig.alarmDwellSec   = 60;

  uint32_t t = 1000000;
  test_setMillis(t);
  s_controlTemp = 23.0f;  // mild deviation starts the dwell
  checkFermenterAlarm(F);

  test_setMillis(t + 30000);
  s_controlTemp = 20.0f;  // back in band - dwell must reset
  checkFermenterAlarm(F);
  TEST_ASSERT_FALSE(g_fermenters[F].alarm);

  // A fresh mild excursion starts its own dwell window from here (t2) -
  // it must NOT fire just because t2 + 60001 is more than 60s after the
  // very first excursion at t.
  uint32_t t2 = t + 31000;
  test_setMillis(t2);
  s_controlTemp = 23.0f;
  checkFermenterAlarm(F);
  TEST_ASSERT_FALSE(g_fermenters[F].alarm);

  test_setMillis(t2 + 59000);  // just under a full dwell since t2
  checkFermenterAlarm(F);
  TEST_ASSERT_FALSE(g_fermenters[F].alarm);

  test_setMillis(t2 + 60001);  // full dwell elapsed since t2
  checkFermenterAlarm(F);
  TEST_ASSERT_TRUE(g_fermenters[F].alarm);
}

// ---- validateFermenterField: bounds/safety checks on remote-settable fields ----
// Guards every write from MQTT/WebAPI to a fermenter's ceiling/floor/hysteresis/
// compressor-delay/OG/TG - a bug here would silently let an unsafe value through.

void test_validate_ceiling_temp_in_range_accepted(void) {
  g_fermenters[F].floorTemp  = 18.0f;
  g_fermenters[F].hysteresis = 0.5f;
  const char* err = nullptr;
  TEST_ASSERT_TRUE(validateFermenterField(F, "ceiling_temperature", 22.0f, &err));
  TEST_ASSERT_NULL(err);
}

void test_validate_ceiling_temp_rejects_below_absolute_range(void) {
  const char* err = nullptr;
  TEST_ASSERT_FALSE(validateFermenterField(F, "ceiling_temperature", -25.0f, &err));
  TEST_ASSERT_EQUAL_STRING("temperature out of range (-20 to 50)", err);
}

void test_validate_ceiling_temp_rejects_above_absolute_range(void) {
  const char* err = nullptr;
  TEST_ASSERT_FALSE(validateFermenterField(F, "ceiling_temperature", 55.0f, &err));
  TEST_ASSERT_EQUAL_STRING("temperature out of range (-20 to 50)", err);
}

void test_validate_floor_temp_rejects_when_at_or_above_ceiling(void) {
  g_fermenters[F].ceilingTemp = 22.0f;
  g_fermenters[F].hysteresis  = 0.5f;
  const char* err = nullptr;
  TEST_ASSERT_FALSE(validateFermenterField(F, "floor_temperature", 22.0f, &err));
  TEST_ASSERT_EQUAL_STRING("floor must be below ceiling", err);
}

void test_validate_temp_rejects_gap_below_2x_hysteresis(void) {
  g_fermenters[F].ceilingTemp = 22.0f;
  g_fermenters[F].hysteresis  = 2.0f;  // needs a >= 4.0 gap
  const char* err = nullptr;
  TEST_ASSERT_FALSE(validateFermenterField(F, "floor_temperature", 20.0f, &err));  // gap 2.0
  TEST_ASSERT_EQUAL_STRING("safe zone must be at least 2x hysteresis", err);
}

void test_validate_hysteresis_in_range_accepted(void) {
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;
  TEST_ASSERT_TRUE(validateFermenterField(F, "hysteresis", 1.0f, nullptr));
}

void test_validate_hysteresis_rejects_out_of_range(void) {
  const char* err = nullptr;
  TEST_ASSERT_FALSE(validateFermenterField(F, "hysteresis", 15.0f, &err));
  TEST_ASSERT_EQUAL_STRING("hysteresis out of range (0 to 10)", err);
}

void test_validate_hysteresis_rejects_when_it_violates_existing_gap(void) {
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;  // existing 4.0 gap
  const char* err = nullptr;
  TEST_ASSERT_FALSE(validateFermenterField(F, "hysteresis", 3.0f, &err));  // needs gap >= 6.0
  TEST_ASSERT_EQUAL_STRING("safe zone must be at least 2x hysteresis", err);
}

void test_validate_compressor_delay_in_range_accepted(void) {
  TEST_ASSERT_TRUE(validateFermenterField(F, "compressor_delay", 30.0f, nullptr));
}

void test_validate_compressor_delay_rejects_out_of_range(void) {
  const char* err = nullptr;
  TEST_ASSERT_FALSE(validateFermenterField(F, "compressor_delay", 1500.0f, &err));
  TEST_ASSERT_EQUAL_STRING("compressor delay out of range (0 to 1440 min)", err);
}

void test_validate_gravity_fields_in_range_accepted(void) {
  TEST_ASSERT_TRUE(validateFermenterField(F, "og", 1.050f, nullptr));
  TEST_ASSERT_TRUE(validateFermenterField(F, "tg", 1.010f, nullptr));
}

void test_validate_gravity_fields_reject_out_of_range(void) {
  const char* err = nullptr;
  TEST_ASSERT_FALSE(validateFermenterField(F, "og", 0.980f, &err));
  TEST_ASSERT_EQUAL_STRING("gravity out of range (0.990 to 1.200)", err);

  err = nullptr;
  TEST_ASSERT_FALSE(validateFermenterField(F, "tg", 1.250f, &err));
  TEST_ASSERT_EQUAL_STRING("gravity out of range (0.990 to 1.200)", err);
}

void test_validate_unrecognised_key_passes_through(void) {
  // Fields with no explicit validation branch (e.g. "name") are accepted as-is -
  // validateFermenterField only guards the numeric safety-critical fields.
  TEST_ASSERT_TRUE(validateFermenterField(F, "name", 0.0f, nullptr));
}

void test_validate_accepts_null_errmsg_pointer(void) {
  // Every rejection path guards its *errMsg write with `if (errMsg)` - callers
  // that don't care about the reason must be able to pass nullptr safely.
  TEST_ASSERT_FALSE(validateFermenterField(F, "hysteresis", 15.0f, nullptr));
}

// ---- processFermenters: top-level dispatch ----
// Two documented behaviours that the per-fermenter functions alone don't show:
// a powered-off fermenter is not alarm-checked at all, while one with temp
// control merely paused still is.

void test_dispatch_powered_off_skips_alarm_entirely(void) {
  g_fermenters[F].power          = false;
  g_fermenters[F].floorTemp      = 18.0f;
  g_fermenters[F].ceilingTemp    = 22.0f;
  g_fermenters[F].alarmTolerance = 3.0f;
  g_fermenters[F].alarm          = false;
  g_smartPlugs[0].fermenter = F;
  g_smartPlugs[0].function  = PLUG_FN_F1_HOT;
  s_state[F] = STATUS_HEATING;   // latched from before it was switched off
  s_controlTemp = 26.0f;         // severe over-temp: WOULD alarm if checked

  processFermenters();

  TEST_ASSERT_FALSE(g_fermenters[F].alarm);   // not evaluated
  TEST_ASSERT_EQUAL(STATUS_IDLE, g_fermenters[F].status);
  TEST_ASSERT_EQUAL(STATUS_IDLE, s_state[F]);
  TEST_ASSERT_EQUAL(0, lastPlugState(0));     // plug forced off
}

void test_dispatch_temp_control_off_still_alarms(void) {
  g_fermenters[F].tempControl    = false;
  g_fermenters[F].floorTemp      = 18.0f;
  g_fermenters[F].ceilingTemp    = 22.0f;
  g_fermenters[F].alarmTolerance = 3.0f;
  g_fermenters[F].alarm          = false;
  g_smartPlugs[0].fermenter = F;
  g_smartPlugs[0].function  = PLUG_FN_F1_HOT;
  s_controlTemp = 26.0f;         // severe over-temp

  processFermenters();

  TEST_ASSERT_TRUE(g_fermenters[F].alarm);    // still watched
  TEST_ASSERT_EQUAL(STATUS_IDLE, g_fermenters[F].status);
  TEST_ASSERT_EQUAL(0, lastPlugState(0));     // but no control action
}

void test_dispatch_controls_normally_when_powered_and_enabled(void) {
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;
  g_smartPlugs[0].fermenter = F;
  g_smartPlugs[0].function  = PLUG_FN_F1_HOT;
  s_controlTemp = 17.0f;

  processFermenters();

  TEST_ASSERT_EQUAL(STATUS_HEATING, g_fermenters[F].status);
  TEST_ASSERT_EQUAL(1, lastPlugState(0));
}

// ---- setFermenterPlugs: the PLUG_FN_F1_HOT + index*2 mapping ----

void test_plug_mapping_for_third_and_fourth_fermenters(void) {
  // Fermenters 2 and 3 use PLUG_FN_F3_* / PLUG_FN_F4_*; the existing mapping
  // test only covers 0 and 1, where the *2 arithmetic barely moves.
  g_smartPlugs[4].fermenter = 2;
  g_smartPlugs[4].function  = PLUG_FN_F3_HOT;
  g_smartPlugs[5].fermenter = 2;
  g_smartPlugs[5].function  = PLUG_FN_F3_COLD;
  g_smartPlugs[6].fermenter = 3;
  g_smartPlugs[6].function  = PLUG_FN_F4_HOT;
  g_smartPlugs[7].fermenter = 3;
  g_smartPlugs[7].function  = PLUG_FN_F4_COLD;

  g_fermenters[2].power       = true;
  g_fermenters[2].tempControl = true;
  g_fermenters[2].floorTemp   = 18.0f;
  g_fermenters[2].ceilingTemp = 22.0f;

  s_controlTemp = 17.0f;  // below floor -> heat fermenter 2
  processSingleFermenter(2);

  TEST_ASSERT_EQUAL(1, lastPlugState(4));   // F3 hot on
  TEST_ASSERT_EQUAL(0, lastPlugState(5));   // F3 cold off
  TEST_ASSERT_EQUAL(-1, lastPlugState(6));  // fermenter 3 untouched
  TEST_ASSERT_EQUAL(-1, lastPlugState(7));
}

// ---- switchOffAll ----

void test_switchOffAll_turns_off_every_plug(void) {
  for (int p = 0; p < MAX_SMART_PLUGS; p++) {
    g_smartPlugs[p].fermenter = 0;
    g_smartPlugs[p].function  = PLUG_FN_F1_HOT;
  }
  switchOffAll();
  for (int p = 0; p < MAX_SMART_PLUGS; p++) {
    TEST_ASSERT_EQUAL(0, lastPlugState(p));
  }
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    TEST_ASSERT_EQUAL(STATUS_IDLE, g_fermenters[i].status);
  }
}

void test_switchOffAll_clears_latched_control_state(void) {
  // The internal state machine must be reset too, not just the plugs -
  // otherwise the next pass resumes HEATING from where it left off.
  g_fermenters[F].floorTemp   = 18.0f;
  g_fermenters[F].ceilingTemp = 22.0f;
  g_fermenters[F].hysteresis  = 2.0f;   // HEATING would only stop at >= 20.0
  s_state[F] = STATUS_HEATING;

  switchOffAll();

  s_controlTemp = 19.0f;  // in band, but below floor + hysteresis
  processSingleFermenter(F);
  // Idle because the state was cleared; a surviving HEATING state would have
  // needed 20.0 to drop out and would still read HEATING here.
  TEST_ASSERT_EQUAL(STATUS_IDLE, g_fermenters[F].status);
}

// ---- setFermenterPower ----

void test_setFermenterPower_off_forces_plugs_off_and_idle(void) {
  g_smartPlugs[0].fermenter = F;
  g_smartPlugs[0].function  = PLUG_FN_F1_HOT;
  g_smartPlugs[1].fermenter = F;
  g_smartPlugs[1].function  = PLUG_FN_F1_COLD;
  g_fermenters[F].status = STATUS_HEATING;

  setFermenterPower(F, false);

  TEST_ASSERT_FALSE(g_fermenters[F].power);
  TEST_ASSERT_EQUAL(STATUS_IDLE, g_fermenters[F].status);
  TEST_ASSERT_EQUAL(0, lastPlugState(0));
  TEST_ASSERT_EQUAL(0, lastPlugState(1));
}

void test_setFermenterPower_ignores_out_of_range_index(void) {
  // Guard against an out-of-bounds write from a bad MQTT/WebAPI index.
  setFermenterPower(MAX_FERMENTERS, false);
  TEST_ASSERT_TRUE(g_fermenters[F].power);   // fermenter 0 untouched
  TEST_ASSERT_EQUAL(0, s_plugCallCount);     // and no plug was switched
}

// ---- estimateGravity: the no-hydrometer fallback model ----
// Flat for 24 h (lag phase), 90% of the OG->TG drop over the next 72 h, the
// last 10% over the 72 h after that, then flat at TG.

void test_estimateGravity_flat_during_lag_phase(void) {
  g_fermenters[F].og = 1.050f;
  g_fermenters[F].tg = 1.010f;
  g_fermenters[F].currentHour = 0;
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.050f, estimateGravity(F));
  g_fermenters[F].currentHour = 24;
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.050f, estimateGravity(F));
}

void test_estimateGravity_mid_primary_phase(void) {
  g_fermenters[F].og = 1.050f;
  g_fermenters[F].tg = 1.010f;
  g_fermenters[F].currentHour = 60;  // halfway through the 24-96 h window
  // 0.5 * 0.9 * 0.040 = 0.018 dropped
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.032f, estimateGravity(F));
}

void test_estimateGravity_at_end_of_primary_phase(void) {
  g_fermenters[F].og = 1.050f;
  g_fermenters[F].tg = 1.010f;
  g_fermenters[F].currentHour = 96;  // 90% of the range gone
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.014f, estimateGravity(F));
}

void test_estimateGravity_reaches_tg_and_clamps(void) {
  g_fermenters[F].og = 1.050f;
  g_fermenters[F].tg = 1.010f;
  g_fermenters[F].currentHour = 168;  // 96 + 72: final 10% complete
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.010f, estimateGravity(F));
  g_fermenters[F].currentHour = 1000;  // must not keep falling past TG
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.010f, estimateGravity(F));
}

void test_estimateGravity_returns_og_when_tg_not_below_og(void) {
  // Misconfigured OG/TG must not produce a rising or inverted estimate.
  g_fermenters[F].og = 1.010f;
  g_fermenters[F].tg = 1.050f;
  g_fermenters[F].currentHour = 100;
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.010f, estimateGravity(F));
}

// ---- getCurrentSG / getGravitySource: Debug > Tilt > iSpindel > Estimate ----
// Value and label come from one shared resolver, so every case asserts both -
// they must not be able to drift apart.

void test_gravity_falls_back_to_estimate(void) {
  g_fermenters[F].og = 1.050f;
  g_fermenters[F].tg = 1.010f;
  g_fermenters[F].currentHour = 60;
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.032f, getCurrentSG(F));
  TEST_ASSERT_EQUAL_STRING("Estimated", getGravitySource(F));
}

void test_gravity_uses_ispindel_when_assigned(void) {
  g_iSpindels[0].collectData = true;
  g_iSpindels[0].fermenter   = F;
  g_iSpindels[0].sg          = 1.040f;
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.040f, getCurrentSG(F));
  TEST_ASSERT_EQUAL_STRING("iSpindel", getGravitySource(F));
}

void test_gravity_prefers_ispindel_corrected_reading(void) {
  // corrGravity is the temperature-corrected value the device reports; it wins
  // over the raw sg whenever it's populated.
  g_iSpindels[0].collectData  = true;
  g_iSpindels[0].fermenter    = F;
  g_iSpindels[0].sg           = 1.040f;
  g_iSpindels[0].corrGravity  = 1.038f;
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.038f, getCurrentSG(F));
}

void test_gravity_tilt_beats_ispindel(void) {
  g_iSpindels[0].collectData = true;
  g_iSpindels[0].fermenter   = F;
  g_iSpindels[0].sg          = 1.040f;

  // A Tilt supplies gravity purely on being active and assigned - unlike the
  // beer-temp chain, its `function` doesn't gate the gravity reading.
  g_tilts[0].active    = true;
  g_tilts[0].fermenter = F;
  g_tilts[0].function  = PROBE_UNASSIGNED;
  g_tilts[0].sg        = 1.036f;

  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.036f, getCurrentSG(F));
  TEST_ASSERT_EQUAL_STRING("Tilt", getGravitySource(F));
}

void test_gravity_debug_override_beats_everything(void) {
  g_tilts[0].active    = true;
  g_tilts[0].fermenter = F;
  g_tilts[0].sg        = 1.036f;

  g_fermenterDebugMode = true;
  g_fermenterDebugOverrides[F].enabled = true;
  g_fermenterDebugOverrides[F].sg      = 1.020f;

  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.020f, getCurrentSG(F));
  TEST_ASSERT_EQUAL_STRING("Debug", getGravitySource(F));
}

void test_gravity_ignores_source_assigned_to_another_fermenter(void) {
  g_fermenters[F].og = 1.050f;
  g_fermenters[F].tg = 1.010f;
  g_fermenters[F].currentHour = 0;

  g_tilts[0].active    = true;
  g_tilts[0].fermenter = 1;          // someone else's Tilt
  g_tilts[0].sg        = 1.036f;
  g_iSpindels[0].collectData = true;
  g_iSpindels[0].fermenter   = 1;
  g_iSpindels[0].sg          = 1.040f;

  TEST_ASSERT_EQUAL_STRING("Estimated", getGravitySource(F));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.050f, getCurrentSG(F));
}

// ---- getAttenuation / getEstABV ----

void test_getAttenuation_normal_case(void) {
  g_fermenters[F].og = 1.050f;
  g_tilts[0].active    = true;
  g_tilts[0].fermenter = F;
  g_tilts[0].sg        = 1.020f;
  // (1.050 - 1.020) / (1.050 - 1.000) = 60%
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, getAttenuation(F));
}

void test_getAttenuation_zero_when_og_not_above_water(void) {
  g_fermenters[F].og = 1.000f;
  g_tilts[0].active    = true;
  g_tilts[0].fermenter = F;
  g_tilts[0].sg        = 0.995f;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, getAttenuation(F));
}

void test_getAttenuation_zero_when_no_gravity_reading(void) {
  // An active Tilt reporting 0.0 means "no reading yet", not "fully attenuated".
  g_fermenters[F].og = 1.050f;
  g_tilts[0].active    = true;
  g_tilts[0].fermenter = F;
  g_tilts[0].sg        = 0.0f;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, getAttenuation(F));
}

void test_getAttenuation_zero_when_sg_at_or_above_og(void) {
  // Start of fermentation (or a mis-set OG) must read 0%, never negative.
  g_fermenters[F].og = 1.050f;
  g_tilts[0].active    = true;
  g_tilts[0].fermenter = F;
  g_tilts[0].sg        = 1.055f;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, getAttenuation(F));
}

void test_getEstABV_normal_case(void) {
  g_fermenters[F].og = 1.050f;
  g_tilts[0].active    = true;
  g_tilts[0].fermenter = F;
  g_tilts[0].sg        = 1.010f;
  // (1.050 - 1.010) * 131.25 = 5.25% ABV
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.25f, getEstABV(F));
}

void test_getEstABV_zero_on_the_same_guards(void) {
  g_fermenters[F].og = 1.050f;
  g_tilts[0].active    = true;
  g_tilts[0].fermenter = F;
  g_tilts[0].sg        = 0.0f;          // no reading
  TEST_ASSERT_EQUAL_FLOAT(0.0f, getEstABV(F));
  g_tilts[0].sg        = 1.060f;        // above OG
  TEST_ASSERT_EQUAL_FLOAT(0.0f, getEstABV(F));
  g_fermenters[F].og   = 1.000f;        // OG not above water
  g_tilts[0].sg        = 0.995f;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, getEstABV(F));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_idle_to_heating_when_below_floor);
  RUN_TEST(test_idle_to_cooling_when_above_ceiling_and_delay_elapsed);
  RUN_TEST(test_idle_blocks_cooling_during_compressor_delay);
  RUN_TEST(test_heating_to_idle_at_floor_plus_hysteresis);
  RUN_TEST(test_cooling_to_idle_at_ceiling_minus_hysteresis);
  RUN_TEST(test_no_reading_forces_plugs_off_and_idle);
  RUN_TEST(test_plug_mapping_is_per_fermenter);

  RUN_TEST(test_alarm_skipped_when_powered_off);
  RUN_TEST(test_alarm_skipped_on_no_reading);
  RUN_TEST(test_alarm_clears_when_in_band);
  RUN_TEST(test_alarm_fires_immediately_on_severe_deviation);
  RUN_TEST(test_alarm_does_not_fire_immediately_on_mild_deviation);
  RUN_TEST(test_alarm_fires_after_dwell_elapses);
  RUN_TEST(test_alarm_dwell_resets_on_return_to_band);

  RUN_TEST(test_validate_ceiling_temp_in_range_accepted);
  RUN_TEST(test_validate_ceiling_temp_rejects_below_absolute_range);
  RUN_TEST(test_validate_ceiling_temp_rejects_above_absolute_range);
  RUN_TEST(test_validate_floor_temp_rejects_when_at_or_above_ceiling);
  RUN_TEST(test_validate_temp_rejects_gap_below_2x_hysteresis);
  RUN_TEST(test_validate_hysteresis_in_range_accepted);
  RUN_TEST(test_validate_hysteresis_rejects_out_of_range);
  RUN_TEST(test_validate_hysteresis_rejects_when_it_violates_existing_gap);
  RUN_TEST(test_validate_compressor_delay_in_range_accepted);
  RUN_TEST(test_validate_compressor_delay_rejects_out_of_range);
  RUN_TEST(test_validate_gravity_fields_in_range_accepted);
  RUN_TEST(test_validate_gravity_fields_reject_out_of_range);
  RUN_TEST(test_validate_unrecognised_key_passes_through);
  RUN_TEST(test_validate_accepts_null_errmsg_pointer);

  RUN_TEST(test_dispatch_powered_off_skips_alarm_entirely);
  RUN_TEST(test_dispatch_temp_control_off_still_alarms);
  RUN_TEST(test_dispatch_controls_normally_when_powered_and_enabled);

  RUN_TEST(test_plug_mapping_for_third_and_fourth_fermenters);

  RUN_TEST(test_switchOffAll_turns_off_every_plug);
  RUN_TEST(test_switchOffAll_clears_latched_control_state);

  RUN_TEST(test_setFermenterPower_off_forces_plugs_off_and_idle);
  RUN_TEST(test_setFermenterPower_ignores_out_of_range_index);

  RUN_TEST(test_estimateGravity_flat_during_lag_phase);
  RUN_TEST(test_estimateGravity_mid_primary_phase);
  RUN_TEST(test_estimateGravity_at_end_of_primary_phase);
  RUN_TEST(test_estimateGravity_reaches_tg_and_clamps);
  RUN_TEST(test_estimateGravity_returns_og_when_tg_not_below_og);

  RUN_TEST(test_gravity_falls_back_to_estimate);
  RUN_TEST(test_gravity_uses_ispindel_when_assigned);
  RUN_TEST(test_gravity_prefers_ispindel_corrected_reading);
  RUN_TEST(test_gravity_tilt_beats_ispindel);
  RUN_TEST(test_gravity_debug_override_beats_everything);
  RUN_TEST(test_gravity_ignores_source_assigned_to_another_fermenter);

  RUN_TEST(test_getAttenuation_normal_case);
  RUN_TEST(test_getAttenuation_zero_when_og_not_above_water);
  RUN_TEST(test_getAttenuation_zero_when_no_gravity_reading);
  RUN_TEST(test_getAttenuation_zero_when_sg_at_or_above_og);
  RUN_TEST(test_getEstABV_normal_case);
  RUN_TEST(test_getEstABV_zero_on_the_same_guards);

  return UNITY_END();
}
