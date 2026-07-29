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
  // Clear every step slot, not just slot 0's - the cross-slot tests below write
  // to indices 15/30/59, and a leftover step there would change what a later
  // test's profile sees.
  for (int s = 0; s < MAX_PROFILE_STEPS; s++) g_profileSteps[s] = ProfileStep{};
  s_controlTemp  = TEMP_NONE;
  s_currentSG    = 0.0f;
  s_attenuation  = 0.0f;
  s_millis       = 0;
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

// Flat g_profileSteps index for a (profileNo 1-4, step) pair - mirrors
// Profile.cpp's file-static flatStepIndex().
static uint8_t flat(uint8_t profileNo, uint8_t step) {
  return (profileNo - 1) * MAX_STEPS_PER_PROFILE + step;
}

// Put a fermenter into "profile running" state without going through
// startProfile() (which would reset step/hour).
static void runProfile(uint8_t i, uint8_t profileNo, uint8_t step, uint16_t hour) {
  g_fermenters[i].profileNo      = profileNo;
  g_fermenters[i].profileRunning = true;
  g_fermenters[i].profilePaused  = false;
  g_fermenters[i].currentStep    = step;
  g_fermenters[i].currentHour    = hour;
}

// A finished profile must clear ALL run state, so a saved config is
// indistinguishable from one stopped by hand - see finishProfile().
static void assertProfileFinished(uint8_t i) {
  TEST_ASSERT_FALSE(g_fermenters[i].profileRunning);
  TEST_ASSERT_FALSE(g_fermenters[i].profilePaused);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[i].profileNo);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[i].currentStep);
  TEST_ASSERT_EQUAL_UINT16(0, g_fermenters[i].currentHour);
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

// ---- isStepComplete: unrecognised step type ----

void test_unknown_stepType_falls_back_to_time_only(void) {
  // The switch's default: arm. A step saved by a future firmware (or a
  // corrupted stepType) must still advance on elapsed time rather than
  // stalling the profile forever.
  ProfileStep step = makeStep(/*stepType=*/99, 1.0f, 0, 0, 0);
  g_fermenters[F].currentHour = 23;
  TEST_ASSERT_FALSE(isStepComplete(F, step));
  g_fermenters[F].currentHour = 24;
  TEST_ASSERT_TRUE(isStepComplete(F, step));
}

// ---- advanceProfileStep: the step engine ----
// Drives floor/ceiling from the current step's target, advances on completion,
// and finishes the profile on three separate paths.

void test_advance_sets_band_around_ramp_target(void) {
  runProfile(F, 1, 0, /*hour=*/24);
  // 48 h ramp from 10 to 20 - half elapsed, so the target is 15.
  g_profileSteps[flat(1, 0)] = makeStep(STEP_TEMP_OVER_TIME, 2.0f, 10.0f, 20.0f, 0);
  advanceProfileStep(F);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f - PROFILE_TEMP_BAND, g_fermenters[F].floorTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f + PROFILE_TEMP_BAND, g_fermenters[F].ceilingTemp);
  // 24 h of a 48 h step - not complete, so the step must not advance.
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[F].currentStep);
  TEST_ASSERT_TRUE(g_fermenters[F].profileRunning);
}

void test_advance_sets_band_around_flat_target(void) {
  runProfile(F, 1, 0, /*hour=*/10);
  // startTemp == endTemp, so no ramp - the target is endTemp at any hour.
  g_profileSteps[flat(1, 0)] = makeStep(STEP_TIME_OVER_TEMP, 2.0f, 20.0f, 20.0f, 0);
  advanceProfileStep(F);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f - PROFILE_TEMP_BAND, g_fermenters[F].floorTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.0f + PROFILE_TEMP_BAND, g_fermenters[F].ceilingTemp);
}

void test_advance_free_rise_uses_step_bounds_directly(void) {
  runProfile(F, 1, 0, /*hour=*/0);
  // Free Rise is the one step type that does NOT get a band around a target:
  // startTemp/endTemp ARE the floor/ceiling, giving the beer room to self-heat.
  g_profileSteps[flat(1, 0)] = makeStep(STEP_FREE_RISE, 3.0f, 15.0f, 25.0f, 0);
  advanceProfileStep(F);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15.0f, g_fermenters[F].floorTemp);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, g_fermenters[F].ceilingTemp);
}

void test_advance_moves_to_next_step_and_resets_hour(void) {
  runProfile(F, 1, 0, /*hour=*/24);
  g_profileSteps[flat(1, 0)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 18.0f, 0);  // complete
  g_profileSteps[flat(1, 1)] = makeStep(STEP_TIME_OVER_TEMP, 2.0f, 0, 20.0f, 0);  // next
  advanceProfileStep(F);
  TEST_ASSERT_EQUAL_UINT8(1, g_fermenters[F].currentStep);
  TEST_ASSERT_EQUAL_UINT16(0, g_fermenters[F].currentHour);  // fresh clock for step 2
  TEST_ASSERT_TRUE(g_fermenters[F].profileRunning);
}

void test_advance_finishes_when_next_step_is_empty(void) {
  runProfile(F, 1, 0, /*hour=*/24);
  g_profileSteps[flat(1, 0)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 18.0f, 0);
  // flat(1, 1) left zeroed - a 1-step profile.
  advanceProfileStep(F);
  assertProfileFinished(F);
}

void test_advance_finishes_on_empty_current_step(void) {
  runProfile(F, 1, 0, /*hour=*/0);
  // Nothing configured at all - e.g. the assigned profile was cleared while running.
  advanceProfileStep(F);
  assertProfileFinished(F);
}

void test_advance_finishes_when_last_step_completes(void) {
  uint8_t last = MAX_STEPS_PER_PROFILE - 1;
  runProfile(F, 1, last, /*hour=*/24);
  g_profileSteps[flat(1, last)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 18.0f, 0);
  advanceProfileStep(F);
  assertProfileFinished(F);
}

void test_advance_finishes_when_step_index_past_end(void) {
  // Defensive path: currentStep already out of range (e.g. a config written by
  // a build with a larger MAX_STEPS_PER_PROFILE) must finish, not index off the end.
  runProfile(F, 1, MAX_STEPS_PER_PROFILE, /*hour=*/0);
  advanceProfileStep(F);
  assertProfileFinished(F);
}

// ---- profile slot addressing: profileNo 1-4 map to step blocks 0/15/30/45 ----

void test_advance_reads_steps_from_its_own_profile_slot(void) {
  runProfile(F, 2, 0, /*hour=*/24);
  // Profile 2's first step is complete; profile 1's slot holds a step that
  // would NOT complete. Reading the wrong block would stall here instead.
  g_profileSteps[flat(2, 0)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 18.0f, 0);
  g_profileSteps[flat(2, 1)] = makeStep(STEP_TIME_OVER_TEMP, 5.0f, 0, 20.0f, 0);
  g_profileSteps[flat(1, 0)] = makeStep(STEP_TIME_OVER_TEMP, 9.0f, 0, 30.0f, 0);
  advanceProfileStep(F);
  TEST_ASSERT_EQUAL_UINT8(1, g_fermenters[F].currentStep);
  TEST_ASSERT_TRUE(g_fermenters[F].profileRunning);
}

void test_getProfileTargetTemp_reads_its_own_profile_slot(void) {
  g_fermenters[F].profileRunning = true;
  g_fermenters[F].profileNo      = 3;
  g_fermenters[F].currentStep    = 0;
  g_profileSteps[flat(3, 0)] = makeStep(STEP_TIME_OVER_TEMP, 0, 0, 17.0f, 0);
  g_profileSteps[flat(1, 0)] = makeStep(STEP_TIME_OVER_TEMP, 0, 0, 99.0f, 0);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 17.0f, getProfileTargetTemp(F));
}

void test_getProfileTargetTemp_reaches_last_step_of_last_profile(void) {
  // Highest addressable step: profile 4, step 14 -> flat index 59.
  g_fermenters[F].profileRunning = true;
  g_fermenters[F].profileNo      = MAX_PROFILES;
  g_fermenters[F].currentStep    = MAX_STEPS_PER_PROFILE - 1;
  g_profileSteps[flat(MAX_PROFILES, MAX_STEPS_PER_PROFILE - 1)] =
    makeStep(STEP_TIME_OVER_TEMP, 0, 0, 12.5f, 0);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.5f, getProfileTargetTemp(F));
}

void test_countProfileSteps_counts_from_its_own_slot_base(void) {
  // Slot 0 full, slot 1 holds two steps - counting must start at index 15.
  for (int s = 0; s < MAX_STEPS_PER_PROFILE; s++) {
    g_profileSteps[flat(1, s)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 18.0f, 0);
  }
  g_profileSteps[flat(2, 0)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 18.0f, 0);
  g_profileSteps[flat(2, 1)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 19.0f, 0);
  TEST_ASSERT_EQUAL_UINT8(MAX_STEPS_PER_PROFILE, countProfileSteps(0));
  TEST_ASSERT_EQUAL_UINT8(2, countProfileSteps(1));
}

// ---- nextProfileStep / prevProfileStep: manual step controls ----

void test_nextProfileStep_rejected_in_standard_mode(void) {
  g_fermenters[F].profileNo      = 0;
  g_fermenters[F].profileRunning = true;
  TEST_ASSERT_FALSE(nextProfileStep(F));
}

void test_nextProfileStep_rejected_when_not_running(void) {
  g_fermenters[F].profileNo      = 1;
  g_fermenters[F].profileRunning = false;
  TEST_ASSERT_FALSE(nextProfileStep(F));
}

void test_nextProfileStep_advances_and_resets_hour(void) {
  runProfile(F, 1, 0, /*hour=*/9);
  g_profileSteps[flat(1, 0)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 18.0f, 0);
  g_profileSteps[flat(1, 1)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 19.0f, 0);
  g_profileSteps[flat(1, 2)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 20.0f, 0);
  TEST_ASSERT_TRUE(nextProfileStep(F));
  TEST_ASSERT_EQUAL_UINT8(1, g_fermenters[F].currentStep);
  TEST_ASSERT_EQUAL_UINT16(0, g_fermenters[F].currentHour);
}

void test_nextProfileStep_past_last_step_finishes_profile(void) {
  // Two configured steps, sitting on the last one - "next" ends the profile
  // and reports false so the caller knows there was no step to move to.
  runProfile(F, 1, 1, /*hour=*/5);
  g_profileSteps[flat(1, 0)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 18.0f, 0);
  g_profileSteps[flat(1, 1)] = makeStep(STEP_TIME_OVER_TEMP, 1.0f, 0, 19.0f, 0);
  TEST_ASSERT_FALSE(nextProfileStep(F));
  assertProfileFinished(F);
}

void test_prevProfileStep_rejected_at_first_step(void) {
  runProfile(F, 1, 0, /*hour=*/5);
  TEST_ASSERT_FALSE(prevProfileStep(F));
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[F].currentStep);  // no uint8 underflow
  TEST_ASSERT_EQUAL_UINT16(5, g_fermenters[F].currentHour); // and no hour reset
}

void test_prevProfileStep_rejected_when_not_running(void) {
  g_fermenters[F].profileNo      = 1;
  g_fermenters[F].profileRunning = false;
  g_fermenters[F].currentStep    = 2;
  TEST_ASSERT_FALSE(prevProfileStep(F));
  TEST_ASSERT_EQUAL_UINT8(2, g_fermenters[F].currentStep);
}

void test_prevProfileStep_retreats_and_resets_hour(void) {
  runProfile(F, 1, 2, /*hour=*/9);
  TEST_ASSERT_TRUE(prevProfileStep(F));
  TEST_ASSERT_EQUAL_UINT8(1, g_fermenters[F].currentStep);
  TEST_ASSERT_EQUAL_UINT16(0, g_fermenters[F].currentHour);
}

// ---- start / stop / pause / resume ----

void test_startProfile_initialises_run_state(void) {
  test_setMillis(1234567);
  g_fermenters[F].currentStep = 7;    // stale state from a previous run
  g_fermenters[F].currentHour = 40;
  startProfile(F, 2);
  TEST_ASSERT_EQUAL_UINT8(2, g_fermenters[F].profileNo);
  TEST_ASSERT_EQUAL_UINT8(0, g_fermenters[F].currentStep);
  TEST_ASSERT_EQUAL_UINT16(0, g_fermenters[F].currentHour);
  TEST_ASSERT_TRUE(g_fermenters[F].profileRunning);
  TEST_ASSERT_FALSE(g_fermenters[F].profilePaused);
  TEST_ASSERT_EQUAL_UINT32(1234567, g_fermenters[F].startMillis);
}

void test_pauseProfile_preserves_position(void) {
  // The whole point of pause vs. stop: step and elapsed hours survive so
  // resume picks up where it left off instead of restarting the batch.
  runProfile(F, 2, 3, /*hour=*/30);
  pauseProfile(F);
  TEST_ASSERT_FALSE(g_fermenters[F].profileRunning);
  TEST_ASSERT_TRUE(g_fermenters[F].profilePaused);
  TEST_ASSERT_EQUAL_UINT8(2, g_fermenters[F].profileNo);
  TEST_ASSERT_EQUAL_UINT8(3, g_fermenters[F].currentStep);
  TEST_ASSERT_EQUAL_UINT16(30, g_fermenters[F].currentHour);
}

void test_resumeProfile_restores_from_pause(void) {
  runProfile(F, 2, 3, /*hour=*/30);
  pauseProfile(F);
  resumeProfile(F);
  TEST_ASSERT_TRUE(g_fermenters[F].profileRunning);
  TEST_ASSERT_FALSE(g_fermenters[F].profilePaused);
  TEST_ASSERT_EQUAL_UINT8(3, g_fermenters[F].currentStep);
  TEST_ASSERT_EQUAL_UINT16(30, g_fermenters[F].currentHour);
}

void test_resumeProfile_ignored_in_standard_mode(void) {
  g_fermenters[F].profileNo     = 0;
  g_fermenters[F].profilePaused = true;
  resumeProfile(F);
  TEST_ASSERT_FALSE(g_fermenters[F].profileRunning);
  TEST_ASSERT_TRUE(g_fermenters[F].profilePaused);  // untouched - guard returned early
}

void test_resumeProfile_ignored_when_already_running(void) {
  runProfile(F, 1, 0, /*hour=*/0);
  g_fermenters[F].profilePaused = true;  // inconsistent state a resume must not "fix"
  resumeProfile(F);
  TEST_ASSERT_TRUE(g_fermenters[F].profilePaused);
}

void test_stopProfile_clears_everything(void) {
  runProfile(F, 2, 3, /*hour=*/30);
  stopProfile(F);
  assertProfileFinished(F);
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

  RUN_TEST(test_unknown_stepType_falls_back_to_time_only);

  RUN_TEST(test_advance_sets_band_around_ramp_target);
  RUN_TEST(test_advance_sets_band_around_flat_target);
  RUN_TEST(test_advance_free_rise_uses_step_bounds_directly);
  RUN_TEST(test_advance_moves_to_next_step_and_resets_hour);
  RUN_TEST(test_advance_finishes_when_next_step_is_empty);
  RUN_TEST(test_advance_finishes_on_empty_current_step);
  RUN_TEST(test_advance_finishes_when_last_step_completes);
  RUN_TEST(test_advance_finishes_when_step_index_past_end);

  RUN_TEST(test_advance_reads_steps_from_its_own_profile_slot);
  RUN_TEST(test_getProfileTargetTemp_reads_its_own_profile_slot);
  RUN_TEST(test_getProfileTargetTemp_reaches_last_step_of_last_profile);
  RUN_TEST(test_countProfileSteps_counts_from_its_own_slot_base);

  RUN_TEST(test_nextProfileStep_rejected_in_standard_mode);
  RUN_TEST(test_nextProfileStep_rejected_when_not_running);
  RUN_TEST(test_nextProfileStep_advances_and_resets_hour);
  RUN_TEST(test_nextProfileStep_past_last_step_finishes_profile);
  RUN_TEST(test_prevProfileStep_rejected_at_first_step);
  RUN_TEST(test_prevProfileStep_rejected_when_not_running);
  RUN_TEST(test_prevProfileStep_retreats_and_resets_hour);

  RUN_TEST(test_startProfile_initialises_run_state);
  RUN_TEST(test_pauseProfile_preserves_position);
  RUN_TEST(test_resumeProfile_restores_from_pause);
  RUN_TEST(test_resumeProfile_ignored_in_standard_mode);
  RUN_TEST(test_resumeProfile_ignored_when_already_running);
  RUN_TEST(test_stopProfile_clears_everything);

  return UNITY_END();
}
