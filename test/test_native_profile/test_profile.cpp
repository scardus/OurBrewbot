// Native (host) tests for isStepComplete() in OurBrewbot/Profile.cpp.
//
// Profile.cpp is #included directly (not linked) so the real, unmodified
// production source is what's under test - see test/stubs/ for the minimal
// Arduino-core stand-ins that let it compile on the host, and below for the
// storage/test-doubles the rest of the firmware would normally provide.

#include <unity.h>
#include <cstdint>

#include "../../OurBrewbot/Config.h"

// ---- storage Config.h declares extern, normally defined in Config.cpp ----
FermenterConfig g_fermenters[MAX_FERMENTERS];
ProfileStep     g_profileSteps[MAX_PROFILE_STEPS];

// ---- millis(), settable per test ----
static uint32_t s_millis = 0;
uint32_t millis() { return s_millis; }
void test_setMillis(uint32_t ms) { s_millis = ms; }

// ---- no-op persistence stub (isStepComplete's callers save on every step
// change; native tests never touch LittleFS) ----
bool saveFermenterConfig() { return true; }

// ---- no-op logging stub (real logMsgImpl writes to Serial/syslog) ----
void logMsgImpl(uint8_t, PGM_P, ...) {}

// ---- sensor test doubles, settable per test ----
static float s_controlTemp;
static float s_currentSG;
static float s_attenuation;
float getControlTemp(uint8_t) { return s_controlTemp; }
float getCurrentSG(uint8_t)   { return s_currentSG; }
float getAttenuation(uint8_t) { return s_attenuation; }

// The function under test, plus everything else in Profile.cpp.
#include "../../OurBrewbot/Profile.cpp"

// ---- test fixture ----

static const uint8_t F = 0;  // fermenter index used by every test

void setUp(void) {
  g_fermenters[F] = FermenterConfig{};
  s_controlTemp  = TEMP_NONE;
  s_currentSG    = 0.0f;
  s_attenuation  = 0.0f;
}

void tearDown(void) {}

static ProfileStep makeStep(uint8_t stepType, float days, float startTemp,
                             float endTemp, float sgTrigger) {
  ProfileStep step{};
  step.stepType  = stepType;
  step.days      = days;
  step.startTemp = startTemp;
  step.endTemp   = endTemp;
  step.sgTrigger = sgTrigger;
  return step;
}

// ---- STEP_TEMP_OVER_TIME: days elapsed AND temp at target ----

void test_TempOverTime_completes_when_days_elapsed_and_temp_at_target(void) {
  ProfileStep step = makeStep(STEP_TEMP_OVER_TIME, 1.0f, 18.0f, 20.0f, 0);
  g_fermenters[F].currentHour = 24;
  s_controlTemp = 20.2f;  // within PROFILE_TEMP_BAND (0.5)
  TEST_ASSERT_TRUE(isStepComplete(F, step));
}

void test_TempOverTime_blocks_before_days_elapsed(void) {
  ProfileStep step = makeStep(STEP_TEMP_OVER_TIME, 1.0f, 18.0f, 20.0f, 0);
  g_fermenters[F].currentHour = 23;
  s_controlTemp = 20.0f;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

void test_TempOverTime_blocks_when_temp_outside_band(void) {
  ProfileStep step = makeStep(STEP_TEMP_OVER_TIME, 1.0f, 18.0f, 20.0f, 0);
  g_fermenters[F].currentHour = 24;
  s_controlTemp = 21.0f;  // 1.0 away, outside 0.5 band
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

void test_TempOverTime_blocks_on_disconnected_probe_sentinel(void) {
  ProfileStep step = makeStep(STEP_TEMP_OVER_TIME, 1.0f, 18.0f, 20.0f, 0);
  g_fermenters[F].currentHour = 24;
  s_controlTemp = TEMP_NONE;  // -127, below TEMP_VALID_MIN guard
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

// ---- STEP_TIME_OVER_TEMP: days elapsed only ----

void test_TimeOverTemp_completes_when_days_elapsed(void) {
  ProfileStep step = makeStep(STEP_TIME_OVER_TEMP, 2.0f, 0, 0, 0);
  g_fermenters[F].currentHour = 48;
  TEST_ASSERT_TRUE(isStepComplete(F, step));
}

void test_TimeOverTemp_blocks_before_days_elapsed(void) {
  ProfileStep step = makeStep(STEP_TIME_OVER_TEMP, 2.0f, 0, 0, 0);
  g_fermenters[F].currentHour = 47;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

// ---- STEP_FREE_RISE: days elapsed only ----

void test_FreeRise_completes_when_days_elapsed(void) {
  ProfileStep step = makeStep(STEP_FREE_RISE, 3.0f, 15.0f, 25.0f, 0);
  g_fermenters[F].currentHour = 72;
  TEST_ASSERT_TRUE(isStepComplete(F, step));
}

void test_FreeRise_blocks_before_days_elapsed(void) {
  ProfileStep step = makeStep(STEP_FREE_RISE, 3.0f, 15.0f, 25.0f, 0);
  g_fermenters[F].currentHour = 71;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

// ---- STEP_SPECIFIC_GRAVITY: days elapsed AND sg at/below trigger ----

void test_SpecificGravity_completes_when_days_elapsed_and_sg_at_trigger(void) {
  ProfileStep step = makeStep(STEP_SPECIFIC_GRAVITY, 1.0f, 0, 0, 1.010f);
  g_fermenters[F].currentHour = 24;
  s_currentSG = 1.008f;
  TEST_ASSERT_TRUE(isStepComplete(F, step));
}

void test_SpecificGravity_blocks_before_days_elapsed(void) {
  ProfileStep step = makeStep(STEP_SPECIFIC_GRAVITY, 1.0f, 0, 0, 1.010f);
  g_fermenters[F].currentHour = 23;
  s_currentSG = 1.008f;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

void test_SpecificGravity_blocks_when_sg_above_trigger(void) {
  ProfileStep step = makeStep(STEP_SPECIFIC_GRAVITY, 1.0f, 0, 0, 1.010f);
  g_fermenters[F].currentHour = 24;
  s_currentSG = 1.020f;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

void test_SpecificGravity_blocks_on_no_reading_sentinel(void) {
  ProfileStep step = makeStep(STEP_SPECIFIC_GRAVITY, 1.0f, 0, 0, 1.010f);
  g_fermenters[F].currentHour = 24;
  s_currentSG = 0.0f;  // below SG_VALID_MIN guard
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

// ---- STEP_ATTENUATION: days elapsed AND attenuation at/above trigger ----

void test_Attenuation_completes_when_days_elapsed_and_attn_at_trigger(void) {
  ProfileStep step = makeStep(STEP_ATTENUATION, 1.0f, 0, 0, 75.0f);
  g_fermenters[F].currentHour = 24;
  s_attenuation = 80.0f;
  TEST_ASSERT_TRUE(isStepComplete(F, step));
}

void test_Attenuation_blocks_when_attn_below_trigger(void) {
  ProfileStep step = makeStep(STEP_ATTENUATION, 1.0f, 0, 0, 75.0f);
  g_fermenters[F].currentHour = 24;
  s_attenuation = 50.0f;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

// ---- STEP_TEMP_REACHED: temp at target, no time requirement ----

void test_TempReached_completes_immediately_when_temp_at_target(void) {
  ProfileStep step = makeStep(STEP_TEMP_REACHED, 0, 0, 20.0f, 0);
  g_fermenters[F].currentHour = 0;
  s_controlTemp = 19.8f;
  TEST_ASSERT_TRUE(isStepComplete(F, step));
}

void test_TempReached_blocks_when_temp_not_at_target(void) {
  ProfileStep step = makeStep(STEP_TEMP_REACHED, 0, 0, 20.0f, 0);
  s_controlTemp = 10.0f;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

// ---- STEP_SG_REACHED: sg at/below trigger, no time requirement ----

void test_SgReached_completes_when_sg_at_trigger(void) {
  ProfileStep step = makeStep(STEP_SG_REACHED, 0, 0, 0, 1.010f);
  s_currentSG = 1.005f;
  TEST_ASSERT_TRUE(isStepComplete(F, step));
}

void test_SgReached_blocks_on_no_reading_sentinel(void) {
  ProfileStep step = makeStep(STEP_SG_REACHED, 0, 0, 0, 1.010f);
  s_currentSG = 0.0f;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

// ---- STEP_ATTN_REACHED: attn at/above trigger, no time requirement ----

void test_AttnReached_completes_when_attn_at_trigger(void) {
  ProfileStep step = makeStep(STEP_ATTN_REACHED, 0, 0, 0, 75.0f);
  s_attenuation = 75.0f;
  TEST_ASSERT_TRUE(isStepComplete(F, step));
}

void test_AttnReached_blocks_when_attn_below_trigger(void) {
  ProfileStep step = makeStep(STEP_ATTN_REACHED, 0, 0, 0, 75.0f);
  s_attenuation = 74.9f;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

// ---- STEP_TIME_AND_SG: days elapsed AND sg at/below trigger ----

void test_TimeAndSg_completes_when_both_conditions_met(void) {
  ProfileStep step = makeStep(STEP_TIME_AND_SG, 1.0f, 0, 0, 1.010f);
  g_fermenters[F].currentHour = 24;
  s_currentSG = 1.005f;
  TEST_ASSERT_TRUE(isStepComplete(F, step));
}

void test_TimeAndSg_blocks_before_days_elapsed(void) {
  ProfileStep step = makeStep(STEP_TIME_AND_SG, 1.0f, 0, 0, 1.010f);
  g_fermenters[F].currentHour = 23;
  s_currentSG = 1.005f;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

// ---- STEP_TIME_AND_ATTN: days elapsed AND attn at/above trigger ----

void test_TimeAndAttn_completes_when_both_conditions_met(void) {
  ProfileStep step = makeStep(STEP_TIME_AND_ATTN, 1.0f, 0, 0, 75.0f);
  g_fermenters[F].currentHour = 24;
  s_attenuation = 80.0f;
  TEST_ASSERT_TRUE(isStepComplete(F, step));
}

void test_TimeAndAttn_blocks_before_days_elapsed(void) {
  ProfileStep step = makeStep(STEP_TIME_AND_ATTN, 1.0f, 0, 0, 75.0f);
  g_fermenters[F].currentHour = 23;
  s_attenuation = 80.0f;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
}

// ---- getProfileTargetTemp: ramp interpolation + clamping ----

void test_getProfileTargetTemp_none_when_not_running(void) {
  g_fermenters[F].profileRunning = false;
  g_fermenters[F].profileNo      = 1;
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, getProfileTargetTemp(F));
}

void test_getProfileTargetTemp_none_in_standard_mode(void) {
  g_fermenters[F].profileRunning = true;
  g_fermenters[F].profileNo      = 0;
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, getProfileTargetTemp(F));
}

void test_getProfileTargetTemp_none_past_last_step(void) {
  g_fermenters[F].profileRunning = true;
  g_fermenters[F].profileNo      = 1;
  g_fermenters[F].currentStep    = MAX_STEPS_PER_PROFILE;
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NONE, getProfileTargetTemp(F));
}

void test_getProfileTargetTemp_ramp_interpolates_and_clamps(void) {
  g_fermenters[F].profileRunning = true;
  g_fermenters[F].profileNo      = 1;
  g_fermenters[F].currentStep    = 0;
  g_profileSteps[0] = makeStep(STEP_TEMP_OVER_TIME, /*days=*/2.0f, /*start=*/10.0f, /*end=*/20.0f, 0);

  g_fermenters[F].currentHour = 0;   // 0% elapsed (48h ramp)
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, getProfileTargetTemp(F));

  g_fermenters[F].currentHour = 24;  // 50% elapsed
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f, getProfileTargetTemp(F));

  g_fermenters[F].currentHour = 48;  // 100% elapsed
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, getProfileTargetTemp(F));

  g_fermenters[F].currentHour = 96;  // past 100% - must clamp, not overshoot
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, getProfileTargetTemp(F));
}

void test_getProfileTargetTemp_nonramp_returns_endTemp_directly(void) {
  g_fermenters[F].profileRunning = true;
  g_fermenters[F].profileNo      = 1;
  g_fermenters[F].currentStep    = 0;

  // days == 0 - no ramp, regardless of currentHour.
  g_profileSteps[0] = makeStep(STEP_TIME_OVER_TEMP, 0.0f, 10.0f, 20.0f, 0);
  g_fermenters[F].currentHour = 5;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, getProfileTargetTemp(F));

  // startTemp == endTemp - no ramp even with days > 0.
  g_profileSteps[0] = makeStep(STEP_TIME_OVER_TEMP, 2.0f, 20.0f, 20.0f, 0);
  g_fermenters[F].currentHour = 10;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f, getProfileTargetTemp(F));
}

// ---- countProfileSteps ----

void test_countProfileSteps_out_of_range_slot_returns_zero(void) {
  TEST_ASSERT_EQUAL_UINT8(0, countProfileSteps(MAX_PROFILES));
}

void test_countProfileSteps_all_empty_returns_zero(void) {
  for (int s = 0; s < MAX_STEPS_PER_PROFILE; s++) g_profileSteps[s] = ProfileStep{};
  TEST_ASSERT_EQUAL_UINT8(0, countProfileSteps(0));
}

void test_countProfileSteps_counts_until_first_empty(void) {
  for (int s = 0; s < MAX_STEPS_PER_PROFILE; s++) g_profileSteps[s] = ProfileStep{};
  g_profileSteps[0] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 0, 0);
  g_profileSteps[1] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 0, 0);
  g_profileSteps[2] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 0, 0);
  // index 3 left empty (zero-initialised) - counting must stop here.
  TEST_ASSERT_EQUAL_UINT8(3, countProfileSteps(0));
}

void test_countProfileSteps_full_profile(void) {
  for (int s = 0; s < MAX_STEPS_PER_PROFILE; s++) {
    g_profileSteps[s] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 0, 0);
  }
  TEST_ASSERT_EQUAL_UINT8(MAX_STEPS_PER_PROFILE, countProfileSteps(0));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_TempOverTime_completes_when_days_elapsed_and_temp_at_target);
  RUN_TEST(test_TempOverTime_blocks_before_days_elapsed);
  RUN_TEST(test_TempOverTime_blocks_when_temp_outside_band);
  RUN_TEST(test_TempOverTime_blocks_on_disconnected_probe_sentinel);

  RUN_TEST(test_TimeOverTemp_completes_when_days_elapsed);
  RUN_TEST(test_TimeOverTemp_blocks_before_days_elapsed);

  RUN_TEST(test_FreeRise_completes_when_days_elapsed);
  RUN_TEST(test_FreeRise_blocks_before_days_elapsed);

  RUN_TEST(test_SpecificGravity_completes_when_days_elapsed_and_sg_at_trigger);
  RUN_TEST(test_SpecificGravity_blocks_before_days_elapsed);
  RUN_TEST(test_SpecificGravity_blocks_when_sg_above_trigger);
  RUN_TEST(test_SpecificGravity_blocks_on_no_reading_sentinel);

  RUN_TEST(test_Attenuation_completes_when_days_elapsed_and_attn_at_trigger);
  RUN_TEST(test_Attenuation_blocks_when_attn_below_trigger);

  RUN_TEST(test_TempReached_completes_immediately_when_temp_at_target);
  RUN_TEST(test_TempReached_blocks_when_temp_not_at_target);

  RUN_TEST(test_SgReached_completes_when_sg_at_trigger);
  RUN_TEST(test_SgReached_blocks_on_no_reading_sentinel);

  RUN_TEST(test_AttnReached_completes_when_attn_at_trigger);
  RUN_TEST(test_AttnReached_blocks_when_attn_below_trigger);

  RUN_TEST(test_TimeAndSg_completes_when_both_conditions_met);
  RUN_TEST(test_TimeAndSg_blocks_before_days_elapsed);

  RUN_TEST(test_TimeAndAttn_completes_when_both_conditions_met);
  RUN_TEST(test_TimeAndAttn_blocks_before_days_elapsed);

  RUN_TEST(test_getProfileTargetTemp_none_when_not_running);
  RUN_TEST(test_getProfileTargetTemp_none_in_standard_mode);
  RUN_TEST(test_getProfileTargetTemp_none_past_last_step);
  RUN_TEST(test_getProfileTargetTemp_ramp_interpolates_and_clamps);
  RUN_TEST(test_getProfileTargetTemp_nonramp_returns_endTemp_directly);

  RUN_TEST(test_countProfileSteps_out_of_range_slot_returns_zero);
  RUN_TEST(test_countProfileSteps_all_empty_returns_zero);
  RUN_TEST(test_countProfileSteps_counts_until_first_empty);
  RUN_TEST(test_countProfileSteps_full_profile);

  return UNITY_END();
}
