/*
 * Temperatures.cpp — DS18B20 temperature probe implementation
 */

#include "Temperatures.h"
#include "Config.h"
#include "Pins.h"
#include "Log.h"

// Hardware bus instances — defined here, declared extern in Temperatures.h
OneWire        g_oneWireBus1(PIN_ONE_WIRE_BUS1);
DallasTemperature g_sensors1(&g_oneWireBus1);

OneWire        g_oneWireBus2(PIN_ONE_WIRE_BUS2);
DallasTemperature g_sensors2(&g_oneWireBus2);


// ============================================================
// ADDRESS UTILITIES
// ============================================================

String addressToString(const DeviceAddress& addr) {
  // Full 8-byte OneWire address as lowercase hex (16 chars)
  char buf[18];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x%02x%02x",
    addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);
  return String(buf);
}

bool stringToAddress(const char* str, DeviceAddress& addr) {
  if (!str || strlen(str) < 16) return false;
  char buf[3] = {0};
  for (int i = 0; i < 8; i++) {
    buf[0] = str[i*2];
    buf[1] = str[i*2+1];
    addr[i] = (uint8_t)strtol(buf, nullptr, 16);
  }
  return true;
}

// ============================================================
// CLEANUP — remove stale truncated-address duplicates from config
// ============================================================

void cleanupDuplicateProbes() {
  for (int i = 0; i < MAX_PROBES; i++) {
    if (strlen(g_probes[i].address) == 0) continue;

    for (int j = i + 1; j < MAX_PROBES; j++) {
      if (strlen(g_probes[j].address) == 0) continue;

      // Check for exact duplicate or truncated prefix match
      bool isDup = strcasecmp(g_probes[i].address, g_probes[j].address) == 0;
      bool iIsTruncated = !isDup && strlen(g_probes[i].address) < 16 &&
                          strncasecmp(g_probes[j].address, g_probes[i].address, strlen(g_probes[i].address)) == 0;
      bool jIsTruncated = !isDup && strlen(g_probes[j].address) < 16 &&
                          strncasecmp(g_probes[i].address, g_probes[j].address, strlen(g_probes[j].address)) == 0;

      if (!isDup && !iIsTruncated && !jIsTruncated) continue;

      // Decide which to keep: prefer full-length address, then lower index
      int keep = i, remove = j;
      if (iIsTruncated) { keep = j; remove = i; }

      // Transfer any non-default config from the removed entry
      if (g_probes[remove].function != PROBE_UNASSIGNED) {
        g_probes[keep].function  = g_probes[remove].function;
        g_probes[keep].fermenter = g_probes[remove].fermenter;
        g_probes[keep].tempAdjust = g_probes[remove].tempAdjust;
        strlcpy(g_probes[keep].probeName, g_probes[remove].probeName, sizeof(g_probes[keep].probeName));
      }

      logMsg("[TEMP] Removing duplicate probe %s (keeping slot %d)",
        g_probes[remove].address, keep);
      memset(&g_probes[remove], 0, sizeof(ProbeConfig));
      g_probes[remove].function  = PROBE_UNASSIGNED;
      g_probes[remove].fermenter = PROBE_UNASSIGNED;

      // If we removed i, stop checking j's for this i
      if (remove == i) break;
    }
  }
}

// ============================================================
// BUS SCANNING
// ============================================================

void scanBuses() {
  logMsg("[TEMP] Scanning OneWire buses for probes...");

  // Scan Bus 1 (Green Jack)
  g_sensors1.begin();
  int bus1Count = g_sensors1.getDeviceCount();
  logMsg("[TEMP] Bus1 (Green Jack): %d probe(s)", bus1Count);

  // Scan Bus 2 (Black Jack)
  g_sensors2.begin();
  int bus2Count = g_sensors2.getDeviceCount();
  logMsg("[TEMP] Bus2 (Black Jack): %d probe(s)", bus2Count);


  // For each discovered probe, check if it's already in our config
  // If not, add it to the first empty slot
  auto registerProbe = [](DallasTemperature& sensors, int busCount, uint8_t busId) {
    DeviceAddress addr;
    for (int i = 0; i < busCount; i++) {
      if (!sensors.getAddress(addr, i)) continue;
      String addrStr = addressToString(addr);

      // Check if already registered (also match old truncated addresses)
      bool found = false;
      for (int j = 0; j < MAX_PROBES; j++) {
        if (strlen(g_probes[j].address) == 0) continue;
        if (addrStr.equalsIgnoreCase(g_probes[j].address)) {
          g_probes[j].busId = busId;
          found = true;
          break;
        }
        // Match old truncated address (14 chars) as prefix of full address (16 chars)
        if (strlen(g_probes[j].address) < 16 &&
            addrStr.startsWith(g_probes[j].address)) {
          // Upgrade the stored address to full length
          logMsg("[TEMP] Upgrading truncated address %s -> %s",
            g_probes[j].address, addrStr.c_str());
          strlcpy(g_probes[j].address, addrStr.c_str(), sizeof(g_probes[j].address));
          g_probes[j].busId = busId;
          found = true;
          break;
        }
      }

      if (!found) {
        // Find empty slot
        for (int j = 0; j < MAX_PROBES; j++) {
          if (strlen(g_probes[j].address) == 0) {
            strlcpy(g_probes[j].address, addrStr.c_str(), sizeof(g_probes[j].address));
            snprintf(g_probes[j].probeName, sizeof(g_probes[j].probeName), "Probe Bus%d-%d", busId, i+1);
            g_probes[j].function  = PROBE_UNASSIGNED;
            g_probes[j].fermenter = PROBE_UNASSIGNED;
            g_probes[j].busId     = busId;
            logMsg("[TEMP] Found New Probe: %s (Bus%d)", addrStr.c_str(), busId);
            break;
          }
        }
      }
    }
  };

  registerProbe(g_sensors1, bus1Count, 1);
  registerProbe(g_sensors2, bus2Count, 2);
}

// ============================================================
// TEMPERATURE POLLING (async, two-phase)
// ============================================================

// Phase 1: kick off conversion on both buses and return immediately.
// The caller is responsible for waiting at least the conversion time
// (94 << (resolution-9)) ms before calling readTempResults().
void requestTempConversion() {
  g_sensors1.setWaitForConversion(false);
  g_sensors1.requestTemperatures();

  g_sensors2.setWaitForConversion(false);
  g_sensors2.requestTemperatures();
}

// Phase 2: read completed conversion results from all registered probes.
// Must be called only after the conversion time has elapsed.
void readTempResults() {
  DeviceAddress addr;
  for (int i = 0; i < MAX_PROBES; i++) {
    if (strlen(g_probes[i].address) == 0) continue;

    if (!stringToAddress(g_probes[i].address, addr)) continue;

    // Use the bus recorded during the last scan; fall back to both if not yet scanned
    DallasTemperature* sensors = (g_probes[i].busId == 2) ? &g_sensors2 : &g_sensors1;
    float temp = sensors->getTempC(addr);
    if (temp == DEVICE_DISCONNECTED_C && g_probes[i].busId == 0) {
      temp = g_sensors2.getTempC(addr);
    }

    // First-failure blocking retry — catches transient read errors
    if (temp == DEVICE_DISCONNECTED_C && g_probes[i].failCount == 0) {
      sensors->setWaitForConversion(true);
      sensors->requestTemperatures();
      temp = sensors->getTempC(addr);
      sensors->setWaitForConversion(false);
    }

    if (temp != DEVICE_DISCONNECTED_C) {
      g_probes[i].rawTemperature = temp;
      g_probes[i].temperature = temp + g_probes[i].tempAdjust;
      g_probes[i].failCount = 0;
    } else {
      g_probes[i].failCount++;
      if (g_probes[i].failCount <= PROBE_FAIL_THRESHOLD) {
        logMsg("[TEMP] Probe %s not responding (fail %d/%d)",
          g_probes[i].address, g_probes[i].failCount, PROBE_FAIL_THRESHOLD);
      }
      if (g_probes[i].failCount >= PROBE_FAIL_THRESHOLD) {
        g_probes[i].temperature = -127.0f;
        g_probes[i].rawTemperature = -127.0f;
      }
    }
  }
}

// ============================================================
// PERIODIC PROBE SCAN — discover new probes, report inactive
// ============================================================

void periodicProbeScan() {
  int prevCount = 0;
  for (int i = 0; i < MAX_PROBES; i++) {
    if (strlen(g_probes[i].address) > 0) prevCount++;
  }

  scanBuses();
  // begin() inside scanBuses() may reset the library's internal resolution — re-apply it.
  g_sensors1.setResolution(g_globalConfig.resolution);
  g_sensors2.setResolution(g_globalConfig.resolution);

  int newCount = 0;
  for (int i = 0; i < MAX_PROBES; i++) {
    if (strlen(g_probes[i].address) > 0) newCount++;
  }

  if (newCount > prevCount) {
    logMsg("[TEMP] Probe scan: %d new probe(s) detected", newCount - prevCount);
    saveProbeConfig();
  }
}

// ============================================================
// PROBE TEMPERATURE ALLOCATION
// Maps probe readings to fermenter beer/ambient/control slots
// ============================================================

void allocateProbeTemperatures() {
  for (int f = 0; f < MAX_FERMENTERS; f++) {
    if (!g_fermenters[f].power) continue;  // fermenter off

    // Find probes assigned to this fermenter
    for (int p = 0; p < MAX_PROBES; p++) {
      if (g_probes[p].fermenter != f) continue;
      if (strlen(g_probes[p].address) == 0) continue;

      switch (g_probes[p].function) {
        case PROBE_FN_BEER:
          // Beer probe drives the control loop
          break;
        case PROBE_FN_AMBIENT:
          // Ambient probe for fridge/chamber monitoring
          break;
        case PROBE_FN_TILT:
          break;
        case PROBE_FN_ISPINDEL:
          // iSpindel temp handled via getBeerTemp() fallback chain — don't overwrite probe reading
          break;
        default:
          break;
      }
    }
  }
}

// ============================================================
// TEMPERATURE GETTERS
// ============================================================

float getTempByIndex(uint8_t index) {
  if (index >= MAX_PROBES) return -127.0f;
  return g_probes[index].temperature;
}

float getTempQuick(const char* addressStr) {
  DeviceAddress addr;
  if (!stringToAddress(addressStr, addr)) return -127.0f;

  float temp = g_sensors1.getTempC(addr);

  if (temp == DEVICE_DISCONNECTED_C) {
    temp = g_sensors2.getTempC(addr);
  }

  return (temp == DEVICE_DISCONNECTED_C) ? -127.0f : temp;
}

float getBeerTemp(uint8_t fermenterIndex) {
  // Priority 1: Tilt assigned to this fermenter with Beer function
  for (int i = 0; i < MAX_TILTS; i++) {
    if (g_tilts[i].active &&
        g_tilts[i].fermenter == fermenterIndex &&
        g_tilts[i].function  == PROBE_FN_BEER) {
      return g_tilts[i].temperature;
    }
  }
  // Priority 2: DS18B20 Beer probe
  for (int i = 0; i < MAX_PROBES; i++) {
    if (g_probes[i].fermenter == fermenterIndex &&
        g_probes[i].function  == PROBE_FN_BEER &&
        strlen(g_probes[i].address) > 0) {
      return g_probes[i].temperature;
    }
  }
  // Priority 3: iSpindel (only when configured to provide beer temp)
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (g_iSpindels[i].collectData &&
        g_iSpindels[i].fermenter == fermenterIndex &&
        g_iSpindels[i].function  == PROBE_FN_BEER) {
      return g_iSpindels[i].temperature;
    }
  }
  return -127.0f;
}

const char* getBeerTempSource(uint8_t fermenterIndex) {
  // Mirror getBeerTemp() priority chain exactly
  for (int i = 0; i < MAX_TILTS; i++) {
    if (g_tilts[i].active &&
        g_tilts[i].fermenter == fermenterIndex &&
        g_tilts[i].function  == PROBE_FN_BEER) {
      return "Tilt";
    }
  }
  for (int i = 0; i < MAX_PROBES; i++) {
    if (g_probes[i].fermenter == fermenterIndex &&
        g_probes[i].function  == PROBE_FN_BEER &&
        strlen(g_probes[i].address) > 0) {
      return "Probe";
    }
  }
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (g_iSpindels[i].collectData &&
        g_iSpindels[i].fermenter == fermenterIndex &&
        g_iSpindels[i].function  == PROBE_FN_BEER) {
      return "iSpindel";
    }
  }
  return "None";
}

float getAmbientTemp(uint8_t fermenterIndex) {
  for (int i = 0; i < MAX_PROBES; i++) {
    if (g_probes[i].fermenter == fermenterIndex &&
        g_probes[i].function  == PROBE_FN_AMBIENT &&
        strlen(g_probes[i].address) > 0) {
      return g_probes[i].temperature;
    }
  }
  return -127.0f;
}

float getControlTemp(uint8_t fermenterIndex) {
  // Beer probe is preferred for temperature control
  float beer = getBeerTemp(fermenterIndex);
  if (beer > -100.0f) return beer;
  return getAmbientTemp(fermenterIndex);
}

float toDisplayTemp(float celsius) {
  if (g_globalConfig.unit == UNIT_FAHRENHEIT) {
    return celsius * 9.0f / 5.0f + 32.0f;
  }
  return celsius;
}

float toCelsius(float displayTemp) {
  if (g_globalConfig.unit == UNIT_FAHRENHEIT) {
    return (displayTemp - 32.0f) * 5.0f / 9.0f;
  }
  return displayTemp;
}
