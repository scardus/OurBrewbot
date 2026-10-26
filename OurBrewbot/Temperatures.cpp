/*
 * Copyright 2026 Sean Cardus
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Temperatures.cpp — DS18B20 temperature probe implementation
 */

#include "Temperatures.h"
#include "Config.h"
#include "Pins.h"
#include "Log.h"
#include "iSpindel.h"

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

// The original MyBrewbot firmware saved each address byte without its leading
// zero, so 28-80-55-55-0a-00-00-80 is "28805555a0080" in its jsonProbe.txt.
// True when `stored` is that form of `addr`.
bool isUnpaddedAddress(const char* stored, const DeviceAddress& addr) {
  char buf[17];   // 8 bytes of up to 2 hex digits + NUL
  snprintf(buf, sizeof(buf), "%x%x%x%x%x%x%x%x",
    addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);
  return strlen(stored) < 16 && strcasecmp(buf, stored) == 0;
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

bool scanBuses() {
  logMsg("[TEMP] Scanning OneWire buses for probes...");

  // Scan Bus 1 (Green Jack)
  g_sensors1.begin();
  int bus1Count = g_sensors1.getDeviceCount();
  logMsg("[TEMP] Bus1 (Green Jack): %d probe(s)", bus1Count);

  // Scan Bus 2 (Black Jack)
  g_sensors2.begin();
  int bus2Count = g_sensors2.getDeviceCount();
  logMsg("[TEMP] Bus2 (Black Jack): %d probe(s)", bus2Count);

  bool configChanged = false;

  // Register probes found on a bus and assign fixed position-based names.
  // Names are always overwritten so that physically moving a probe to a new
  // bus or position is reflected immediately on the next scan.
  auto registerProbe = [&configChanged](DallasTemperature& sensors, int busCount, uint8_t busId) {
    DeviceAddress addr;
    for (int i = 0; i < busCount; i++) {
      if (!sensors.getAddress(addr, i)) continue;
      String addrStr = addressToString(addr);

      char expectedName[24];
      snprintf(expectedName, sizeof(expectedName), "Probe Bus%d-%d", busId, i + 1);

      bool found = false;
      for (int j = 0; j < MAX_PROBES; j++) {
        if (strlen(g_probes[j].address) == 0) continue;

        bool addrMatch  = addrStr.equalsIgnoreCase(g_probes[j].address);
        bool truncMatch = !addrMatch && strlen(g_probes[j].address) < 16 &&
                          (addrStr.startsWith(g_probes[j].address) ||
                           isUnpaddedAddress(g_probes[j].address, addr));

        if (!addrMatch && !truncMatch) continue;

        if (truncMatch) {
          logMsg("[TEMP] Upgrading truncated address %s -> %s",
            g_probes[j].address, addrStr.c_str());
          strlcpy(g_probes[j].address, addrStr.c_str(), sizeof(g_probes[j].address));
          configChanged = true;
        }

        g_probes[j].busId = busId;

        if (strcmp(g_probes[j].probeName, expectedName) != 0) {
          logMsg("[TEMP] Probe %s renamed: '%s' -> '%s'",
            addrStr.c_str(), g_probes[j].probeName, expectedName);
          strlcpy(g_probes[j].probeName, expectedName, sizeof(g_probes[j].probeName));
          configChanged = true;
        }
        found = true;
        break;
      }

      if (!found) {
        for (int j = 0; j < MAX_PROBES; j++) {
          if (strlen(g_probes[j].address) == 0) {
            strlcpy(g_probes[j].address, addrStr.c_str(), sizeof(g_probes[j].address));
            strlcpy(g_probes[j].probeName, expectedName, sizeof(g_probes[j].probeName));
            g_probes[j].function  = PROBE_UNASSIGNED;
            g_probes[j].fermenter = PROBE_UNASSIGNED;
            g_probes[j].busId     = busId;
            logMsg("[TEMP] New probe: %s (%s)", addrStr.c_str(), expectedName);
            configChanged = true;
            break;
          }
        }
      }
    }
  };

  registerProbe(g_sensors1, bus1Count, 1);
  registerProbe(g_sensors2, bus2Count, 2);

  return configChanged;
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

// 85.0 C is the value a DS18B20 holds from power-on until its first
// conversion, so reading it means the probe reset (e.g. a power glitch on the
// cable) and has no real reading yet. Treat it as a failed read, as the
// original firmware did - the retry below then asks for a fresh conversion.
static const float DS18B20_POWER_ON_C = 85.0f;

static float readProbeC(DallasTemperature& sensors, const DeviceAddress& addr, const char* address) {
  float temp = sensors.getTempC(addr);
  if (temp == DS18B20_POWER_ON_C) {
    logMsgL(SYSLOG_WARNING, "[TEMP] Probe %s read 85.0 C (power-on value) - ignored", address);
    return DEVICE_DISCONNECTED_C;
  }
  return temp;
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
    float temp = readProbeC(*sensors, addr, g_probes[i].address);
    if (temp == DEVICE_DISCONNECTED_C && g_probes[i].busId == 0) {
      temp = readProbeC(g_sensors2, addr, g_probes[i].address);
      if (temp != DEVICE_DISCONNECTED_C) {
        // Remember the bus that served the read so the retry below (and the
        // next cycles until the scan updates it) target the right bus.
        g_probes[i].busId = 2;
        sensors = &g_sensors2;
      }
    }

    // First-failure blocking retry — catches transient read errors on marginal connections.
    // Costs up to 750 ms on first miss per probe per cycle, but avoids false fail-counts.
    if (temp == DEVICE_DISCONNECTED_C && g_probes[i].failCount == 0) {
      sensors->setWaitForConversion(true);
      sensors->requestTemperatures();
      temp = readProbeC(*sensors, addr, g_probes[i].address);
      sensors->setWaitForConversion(false);
      // Probe never seen on a scan (busId 0) and bus 1 retry failed: try bus 2 once.
      if (temp == DEVICE_DISCONNECTED_C && g_probes[i].busId == 0) {
        g_sensors2.setWaitForConversion(true);
        g_sensors2.requestTemperatures();
        temp = readProbeC(g_sensors2, addr, g_probes[i].address);
        g_sensors2.setWaitForConversion(false);
      }
    }

    if (temp != DEVICE_DISCONNECTED_C) {
      g_probes[i].rawTemperature = temp;
      g_probes[i].temperature = temp + g_probes[i].tempAdjust;
      g_probes[i].failCount = 0;
    } else {
      g_probes[i].failCount++;
      if (g_probes[i].failCount <= PROBE_FAIL_THRESHOLD) {
        logMsgL(SYSLOG_WARNING, "[TEMP] Probe %s not responding (fail %u/%d)",
          g_probes[i].address, (unsigned)g_probes[i].failCount, PROBE_FAIL_THRESHOLD);
      } else if (g_probes[i].failCount % PROBE_REMINDER_FAILS == 0) {
        // One plain reminder an hour while the probe stays missing
        logMsgL(SYSLOG_WARNING, "[TEMP] Probe %s still not responding (%u min)",
          g_probes[i].address, (unsigned)(g_probes[i].failCount / 12));
      }
      if (g_probes[i].failCount >= PROBE_FAIL_THRESHOLD) {
        g_probes[i].temperature = TEMP_NONE;
        g_probes[i].rawTemperature = TEMP_NONE;
      }
    }
  }
}

// One full read, blocking for the conversion time. setup() calls this once:
// loop() starts its first conversion and its first control pass in the same
// pass, so without it the first pass would run with no probe read yet (every
// probe starts with no reading - see loadProbeConfig()).
void readTemperaturesNow() {
  requestTempConversion();
  delay(94u << (g_globalConfig.resolution - 9));   // 9 bit = 94 ms ... 12 bit = 752 ms, as in loop()
  readTempResults();
}

// ============================================================
// PERIODIC PROBE SCAN — discover new probes, report inactive
// ============================================================

void periodicProbeScan() {
  bool changed = scanBuses();
  // begin() inside scanBuses() may reset the library's internal resolution — re-apply it.
  g_sensors1.setResolution(g_globalConfig.resolution);
  g_sensors2.setResolution(g_globalConfig.resolution);

  if (changed) {
    logMsg("[TEMP] Probe scan: config updated");
    saveProbeConfig();
  }
}

// ============================================================
// TEMPERATURE GETTERS
// ============================================================

// Single resolver behind getBeerTemp()/getBeerTempSource() — one priority
// chain (Tilt > Probe > iSpindel) shared by both, so the value and its
// source label cannot drift apart. Each source is used only while it has a
// reading, so a failed beer probe falls through to a working iSpindel (and a
// stale iSpindel to nothing, leaving control to the ambient probe). Before
// 0.6.4 the first CONFIGURED source was used whether it had a reading or not.
enum BeerTempSrc : int8_t { BEER_SRC_NONE = -1, BEER_SRC_DEBUG, BEER_SRC_TILT, BEER_SRC_PROBE, BEER_SRC_ISPINDEL };

static BeerTempSrc findBeerTempSource(uint8_t fermenterIndex, int& slot) {
  if (g_fermenterDebugMode && g_fermenterDebugOverrides[fermenterIndex].enabled)
    return BEER_SRC_DEBUG;
  // Priority 1: Tilt assigned to this fermenter with Beer function
  for (int i = 0; i < MAX_TILTS; i++) {
    if (g_tilts[i].active &&
        g_tilts[i].fermenter == fermenterIndex &&
        g_tilts[i].function  == PROBE_FN_BEER &&
        g_tilts[i].temperature > TEMP_VALID_MIN) {
      slot = i;
      return BEER_SRC_TILT;
    }
  }
  // Priority 2: DS18B20 Beer probe
  for (int i = 0; i < MAX_PROBES; i++) {
    if (g_probes[i].fermenter == fermenterIndex &&
        g_probes[i].function  == PROBE_FN_BEER &&
        strlen(g_probes[i].address) > 0 &&
        g_probes[i].temperature > TEMP_VALID_MIN) {
      slot = i;
      return BEER_SRC_PROBE;
    }
  }
  // Priority 3: iSpindel (only when configured to provide beer temp, and only
  // while it is reporting - see iSpindelIsFresh())
  for (int i = 0; i < MAX_ISPINDELS; i++) {
    if (g_iSpindels[i].collectData &&
        g_iSpindels[i].fermenter == fermenterIndex &&
        g_iSpindels[i].function  == PROBE_FN_BEER &&
        iSpindelIsFresh(g_iSpindels[i]) &&
        g_iSpindels[i].temperature > TEMP_VALID_MIN) {
      slot = i;
      return BEER_SRC_ISPINDEL;
    }
  }
  return BEER_SRC_NONE;
}

float getBeerTemp(uint8_t fermenterIndex) {
  int slot = -1;
  switch (findBeerTempSource(fermenterIndex, slot)) {
    case BEER_SRC_DEBUG:    return g_fermenterDebugOverrides[fermenterIndex].beerTemp;
    case BEER_SRC_TILT:     return g_tilts[slot].temperature;
    case BEER_SRC_PROBE:    return g_probes[slot].temperature;
    case BEER_SRC_ISPINDEL: return g_iSpindels[slot].temperature;
    default:                return TEMP_NONE;
  }
}

const char* getBeerTempSource(uint8_t fermenterIndex) {
  int slot = -1;
  switch (findBeerTempSource(fermenterIndex, slot)) {
    case BEER_SRC_DEBUG:    return "Debug";
    case BEER_SRC_TILT:     return "Tilt";
    case BEER_SRC_PROBE:    return "Probe";
    case BEER_SRC_ISPINDEL: return "iSpindel";
    default:
      // No beer reading: control falls back to the ambient probe (see
      // getControlTemp()), so say so rather than "None"
      return (getAmbientTemp(fermenterIndex) > TEMP_VALID_MIN) ? "Ambient" : "None";
  }
}

float getAmbientTemp(uint8_t fermenterIndex) {
  if (g_fermenterDebugMode && g_fermenterDebugOverrides[fermenterIndex].enabled)
    return g_fermenterDebugOverrides[fermenterIndex].ambientTemp;
  for (int i = 0; i < MAX_PROBES; i++) {
    if (g_probes[i].fermenter == fermenterIndex &&
        g_probes[i].function  == PROBE_FN_AMBIENT &&
        strlen(g_probes[i].address) > 0) {
      return g_probes[i].temperature;
    }
  }
  return TEMP_NONE;
}

float getControlTemp(uint8_t fermenterIndex) {
  // Beer probe is preferred for temperature control
  float beer = getBeerTemp(fermenterIndex);
  if (beer > TEMP_VALID_MIN) return beer;
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

// Delta (span) conversions for values like hysteresis — scale only, no +32 offset.
float toDisplayTempDelta(float dCelsius) {
  if (g_globalConfig.unit == UNIT_FAHRENHEIT) {
    return dCelsius * 9.0f / 5.0f;
  }
  return dCelsius;
}

float toCelsiusTempDelta(float dDisplay) {
  if (g_globalConfig.unit == UNIT_FAHRENHEIT) {
    return dDisplay * 5.0f / 9.0f;
  }
  return dDisplay;
}
