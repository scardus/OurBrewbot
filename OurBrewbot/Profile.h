#pragma once
/*
 * Profile.h — Fermentation profile (temperature ramp) controller
 */

#include "Config.h"

// Process all running profiles (called from processFermenters)
void processProfiles();

// Advance a single fermenter's profile step if conditions met
void advanceProfileStep(uint8_t fermenterIndex);

// Check if a profile step's advance condition is satisfied
bool isStepComplete(uint8_t fermenterIndex, const ProfileStep& step);

// Start a profile on a fermenter
void startProfile(uint8_t fermenterIndex, uint8_t profileIndex);

// Stop/pause/resume a profile
void stopProfile(uint8_t fermenterIndex);
void pauseProfile(uint8_t fermenterIndex);
void resumeProfile(uint8_t fermenterIndex);

// Manual step navigation — returns true if the step actually changed
bool nextProfileStep(uint8_t fermenterIndex);
bool prevProfileStep(uint8_t fermenterIndex);

// Count non-empty steps in a profile (0-indexed profileSlot 0-3)
uint8_t countProfileSteps(uint8_t profileSlot);

// Get target temperature for the current profile step
float getProfileTargetTemp(uint8_t fermenterIndex);

// Get description of a step type
const char* getStepTypeDescription(uint8_t stepType);
