/*
 * Profile.cpp — Fermentation profile runner
 *
 * Profile addressing: profileNo 1-4 maps to profile slots 0-3.
 * Each slot has MAX_STEPS_PER_PROFILE (15) steps in the flat g_profileSteps array.
 * Flat index = (profileNo - 1) * MAX_STEPS_PER_PROFILE + currentStep
 */

#include "Profile.h"
#include "Fermenter.h"
#include "Temperatures.h"
#include "Log.h"

// Helper: get flat array index for a fermenter's current profile step
static inline uint8_t flatStepIndex(uint8_t i) {
  return (g_fermenters[i].profileNo - 1) * MAX_STEPS_PER_PROFILE + g_fermenters[i].currentStep;
}

// Helper: check if a step is empty (unused slot)
static inline bool isStepEmpty(const ProfileStep& step) {
  return step.stepType == 0 && step.days == 0 && step.endTemp == 0.0f && step.startTemp == 0.0f;
}

void processProfiles() {
  for (int i = 0; i < MAX_FERMENTERS; i++) {
    if (g_fermenters[i].profileNo == 0) continue;      // Standard mode
    if (!g_fermenters[i].profileRunning) continue;
    if (!g_fermenters[i].power) continue;
    advanceProfileStep(i);
  }
}

void advanceProfileStep(uint8_t i) {
  if (g_fermenters[i].currentStep >= MAX_STEPS_PER_PROFILE) {
    logMsg("[PROF] F%d (%s): Profile Finished", i, g_fermenters[i].fermenterName);
    g_fermenters[i].profileRunning = false;
    saveFermenterConfig();
    return;
  }

  uint8_t idx = flatStepIndex(i);
  ProfileStep& step = g_profileSteps[idx];

  // If the current step is empty, profile is done
  if (isStepEmpty(step)) {
    logMsg("[PROF] F%d (%s): Profile Finished (empty step %d)", i, g_fermenters[i].fermenterName, g_fermenters[i].currentStep);
    g_fermenters[i].profileRunning = false;
    saveFermenterConfig();
    return;
  }

  // Update CeilingTemp/FloorTemp from current step's target
  float target = getProfileTargetTemp(i);
  if (target > -100.0f) {
    g_fermenters[i].floorTemp   = target - 0.5f;
    g_fermenters[i].ceilingTemp = target + 0.5f;
  }

  // Check if step completion conditions are met
  if (isStepComplete(i, step)) {
    logMsg("[PROF] F%d (%s): Step %d complete -> Step %d",
      i, g_fermenters[i].fermenterName, g_fermenters[i].currentStep, g_fermenters[i].currentStep + 1);
    g_fermenters[i].currentStep++;
    g_fermenters[i].currentHour = 0;

    // Check for profile end
    if (g_fermenters[i].currentStep >= MAX_STEPS_PER_PROFILE) {
      logMsg("[PROF] F%d (%s): Profile Finished", i, g_fermenters[i].fermenterName);
      g_fermenters[i].profileRunning = false;
    } else {
      uint8_t nextIdx = flatStepIndex(i);
      if (isStepEmpty(g_profileSteps[nextIdx])) {
        logMsg("[PROF] F%d (%s): Profile Finished (no more steps)", i, g_fermenters[i].fermenterName);
        g_fermenters[i].profileRunning = false;
      }
    }
    saveFermenterConfig();
  }
}

bool isStepComplete(uint8_t i, const ProfileStep& step) {
  float temp = getControlTemp(i);
  float sg   = getCurrentSG(i);
  float attn = getAttenuation(i);
  uint16_t hours = g_fermenters[i].currentHour;
  uint16_t daysInHours = step.days * 24;

  switch (step.stepType) {
    case STEP_TIME_OVER_TEMP:
      // Days elapsed AND temp at target
      if (hours < daysInHours) return false;
      if (temp < -100.0f) return false;
      if (fabs(temp - step.endTemp) > 0.5f) {
        logMsg("[PROF] Profile stalled on Time/Temp: %.1f != %.1f", temp, step.endTemp);
        return false;
      }
      return true;

    case STEP_TEMP_OVER_TIME:
      // Days elapsed
      return hours >= daysInHours;

    case STEP_FREE_RISE:
      // Days elapsed (free rise — no temp target)
      return hours >= daysInHours;

    case STEP_SPECIFIC_GRAVITY:
      // SG at or below trigger
      if (sg <= 0.5f) return false;
      if (sg <= step.sgTrigger) {
        logMsg("[PROF] SG Target reached: %.4f <= %.4f", sg, step.sgTrigger);
        return true;
      }
      // Check for stall
      if (hours > daysInHours * 2) {
        logMsg("[PROF] Profile stalled on SG step");
      }
      return false;

    case STEP_ATTENUATION:
      // Attenuation % at target
      return attn >= step.sgTrigger;

    case STEP_TEMP_REACHED:
      // Temperature reached target
      if (temp < -100.0f) return false;
      if (fabs(temp - step.endTemp) <= 0.5f) {
        logMsg("[PROF] Temp target reached: %.1f", temp);
        return true;
      }
      return false;

    case STEP_SG_REACHED:
      if (sg <= 0.5f) return false;
      return sg <= step.sgTrigger;

    case STEP_ATTN_REACHED:
      return attn >= step.sgTrigger;

    case STEP_TIME_AND_SG:
      if (hours < daysInHours) return false;
      return (sg > 0.5f && sg <= step.sgTrigger);

    case STEP_TIME_AND_ATTN:
      if (hours < daysInHours) return false;
      return attn >= step.sgTrigger;

    default:
      return hours >= daysInHours;
  }
}

float getProfileTargetTemp(uint8_t i) {
  if (!g_fermenters[i].profileRunning) return -127.0f;
  if (g_fermenters[i].profileNo == 0) return -127.0f;
  if (g_fermenters[i].currentStep >= MAX_STEPS_PER_PROFILE) return -127.0f;

  uint8_t idx = flatStepIndex(i);
  ProfileStep& step = g_profileSteps[idx];

  // For ramp steps, interpolate between startTemp and endTemp
  if (step.days > 0 && step.startTemp != step.endTemp) {
    float progress = min(1.0f, (float)g_fermenters[i].currentHour / (step.days * 24.0f));
    return step.startTemp + (step.endTemp - step.startTemp) * progress;
  }
  return step.endTemp;
}

void startProfile(uint8_t i, uint8_t profileIndex) {
  g_fermenters[i].profileNo      = profileIndex;  // 1-4
  g_fermenters[i].currentStep    = 0;
  g_fermenters[i].currentHour    = 0;
  g_fermenters[i].profileRunning = true;
  g_fermenters[i].startMillis    = millis();
  logMsg("[PROF] F%d (%s): Profile %d started", i, g_fermenters[i].fermenterName, profileIndex);
  saveFermenterConfig();
}

void stopProfile(uint8_t i) {
  logMsg("[PROF] F%d (%s): Profile stopped", i, g_fermenters[i].fermenterName);
  g_fermenters[i].profileRunning = false;
  g_fermenters[i].profileNo      = 0;
  g_fermenters[i].currentStep    = 0;
  g_fermenters[i].currentHour    = 0;
  saveFermenterConfig();
}

void pauseProfile(uint8_t i) {
  logMsg("[PROF] F%d (%s): Profile Paused", i, g_fermenters[i].fermenterName);
  // Preserve profileNo, currentStep, currentHour for resume
  g_fermenters[i].profileRunning = false;
  saveFermenterConfig();
}

void nextProfileStep(uint8_t i) {
  if (g_fermenters[i].profileNo == 0) return;
  if (!g_fermenters[i].profileRunning) return;

  uint8_t maxStep = countProfileSteps(g_fermenters[i].profileNo - 1);
  if (g_fermenters[i].currentStep + 1 >= maxStep) {
    // Past the end — finish the profile
    logMsg("[PROF] F%d (%s): Next step past end -> Profile Finished", i, g_fermenters[i].fermenterName);
    g_fermenters[i].profileRunning = false;
    saveFermenterConfig();
    return;
  }
  g_fermenters[i].currentStep++;
  g_fermenters[i].currentHour = 0;
  logMsg("[PROF] F%d (%s): Manual advance to step %d", i, g_fermenters[i].fermenterName, g_fermenters[i].currentStep);
  saveFermenterConfig();
}

void prevProfileStep(uint8_t i) {
  if (g_fermenters[i].profileNo == 0) return;
  if (!g_fermenters[i].profileRunning) return;
  if (g_fermenters[i].currentStep == 0) return;

  g_fermenters[i].currentStep--;
  g_fermenters[i].currentHour = 0;
  logMsg("[PROF] F%d (%s): Manual retreat to step %d", i, g_fermenters[i].fermenterName, g_fermenters[i].currentStep);
  saveFermenterConfig();
}

uint8_t countProfileSteps(uint8_t profileSlot) {
  if (profileSlot >= MAX_PROFILES) return 0;
  uint8_t base = profileSlot * MAX_STEPS_PER_PROFILE;
  uint8_t count = 0;
  for (uint8_t s = 0; s < MAX_STEPS_PER_PROFILE; s++) {
    if (isStepEmpty(g_profileSteps[base + s])) break;
    count++;
  }
  return count;
}

const char* getStepTypeDescription(uint8_t stepType) {
  switch (stepType) {
    case STEP_TIME_OVER_TEMP:   return "Time over Temperature Step";
    case STEP_TEMP_OVER_TIME:   return "Temperature over Time Step";
    case STEP_FREE_RISE:        return "Free Rise Step";
    case STEP_SPECIFIC_GRAVITY: return "Specific Gravity Step";
    case STEP_ATTENUATION:      return "Attenuation % Step";
    case STEP_TEMP_REACHED:     return "Temp Reached Step";
    case STEP_SG_REACHED:       return "SG Reached Step";
    case STEP_ATTN_REACHED:     return "Attn% Reached Step";
    case STEP_TIME_AND_SG:      return "Time and SG Step";
    case STEP_TIME_AND_ATTN:    return "Time and Attn% Step";
    default:                    return "Unknown Step";
  }
}
