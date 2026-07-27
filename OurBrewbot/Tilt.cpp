/*
 * Tilt.cpp — Tilt hydrometer support via HM-10 BLE module
 *
 * KeyeStudio Bluetooth 4.0 v2 (HM-10 / CC2541 compatible)
 * communicates via SoftwareSerial AT commands.
 *
 * AT+DISI? returns iBeacon advertisements in colon-delimited format:
 *   OK+DISC:CompanyID:UUID:MajorMinorPower:MACAddr:RSSI
 *   Example: OK+DISC:004C0215:A495BB10C5B14B44B5121370F02D74DE:0044041AF6:F42DC96DA4F2:-053
 *   Where: 004C0215 = Apple iBeacon company ID
 *          A495BBx0...74DE = Tilt UUID (x = colour: 1=Red..8=Pink)
 *          0044 = Major (temp °F, hex)
 *          041A = Minor (SG × 1000, hex)
 *          F6 = measured power (RSSI)
 *
 * Legacy concatenated format (older firmware) is also supported as a fallback:
 *   OK+DISC:00000000:00000000:4C000215A495BB10C5B14B44B5121370F02D74DE00E703F0C5
 */

#include "Tilt.h"
#include "Pins.h"
#include "Log.h"
#include "Reports.h"   // reportsPending() — Tilt scan defers to an in-flight cloud report

// SoftwareSerial: RX = GPIO13 (D7, BLE TX), TX = GPIO12 (D6, BLE RX)
SoftwareSerial g_bleSerial(PIN_BLE_RX, PIN_BLE_TX);

// Standard Tilt iBeacon UUIDs by colour (without hyphens, uppercase)
// The distinguishing byte is at position 7 (1=Red, 2=Green, ... 8=Pink)
static const char* TILT_UUIDS[] = {
  "a495bb10-c5b1-4b44-b512-1370f02d74de",  // Red
  "a495bb20-c5b1-4b44-b512-1370f02d74de",  // Green
  "a495bb30-c5b1-4b44-b512-1370f02d74de",  // Black
  "a495bb40-c5b1-4b44-b512-1370f02d74de",  // Purple
  "a495bb50-c5b1-4b44-b512-1370f02d74de",  // Orange
  "a495bb60-c5b1-4b44-b512-1370f02d74de",  // Blue
  "a495bb70-c5b1-4b44-b512-1370f02d74de",  // Yellow
  "a495bb80-c5b1-4b44-b512-1370f02d74de",  // Pink
};

// UUID match bytes for quick detection (position 14-15 in hex string: "BB10", "BB20", etc.)
static const char TILT_UUID_BYTES[] = { '1', '2', '3', '4', '5', '6', '7', '8' };

static const char* TILT_COLOUR_NAMES[] = {
  "Red", "Green", "Black", "Purple", "Orange", "Blue", "Yellow", "Pink"
};

// Track missed reads per colour for deregistration logic
static int s_missedReads[MAX_TILTS] = {0};

// BLE module state
static bool s_bleReady = false;
bool g_bleSniffActive = false;           // set by BLE sniff page to pause Tilt scanning
#define BLE_BUF_SIZE 320          // longest AT+DISI? response observed ~280 chars
static char  s_bleBuf[BLE_BUF_SIZE];
static int   s_bleBufLen = 0;

// Non-blocking scan state: startTiltScan() kicks off AT+DISI? on the 5 s tick,
// serviceTilt() drains the response across loop passes until OK+DISCE or timeout.
static bool          s_scanActive = false;
static unsigned long s_scanStart  = 0;
static unsigned long s_lastBleInitRetry = 0;
#define BLE_SCAN_TIMEOUT_MS 4000       // same ceiling the old busy-wait used
#define BLE_REINIT_RETRY_MS 300000UL   // retry a failed HM-10 init every 5 min

// Parse a 4-char hex string to uint16_t.
// Returns false if ANY of the 4 characters is not hex — a garbled frame must be
// rejected, not silently turned into a plausible-looking number.
static bool hexToU16(const char* hex, uint16_t* out) {
  uint16_t val = 0;
  for (int i = 0; i < 4; i++) {
    val <<= 4;
    char c = hex[i];
    if      (c >= '0' && c <= '9') val |= (c - '0');
    else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
    else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
    else return false;
  }
  *out = val;
  return true;
}

// The 24 UUID characters that follow the colour byte. Identical for every Tilt
// colour and model, so verifying them catches a frame corrupted after the
// colour byte — which a prefix-only check would happily accept as valid.
static const char TILT_UUID_TAIL[] = "C5B14B44B5121370F02D74DE";

// Identify a Tilt from a 32-char iBeacon UUID hex string (no leading company ID).
// Verifies the WHOLE UUID: "A495BB" + colour digit + '0' + fixed 24-char tail.
// Returns colour index (0-7), or -1 if this is not a Tilt.
static int identifyTiltUuid(const char* uuidHex) {
  if (strlen(uuidHex) < 32) return -1;
  if (strncasecmp(uuidHex, "A495BB", 6) != 0) return -1;
  if (uuidHex[7] != '0') return -1;              // pattern is A495BBx0
  if (strncasecmp(uuidHex + 8, TILT_UUID_TAIL, 24) != 0) return -1;

  char colourByte = uuidHex[6];                  // The distinguishing digit
  for (int i = 0; i < MAX_TILTS; i++) {
    if (colourByte == TILT_UUID_BYTES[i]) return i;
  }
  return -1;
}

// Decode and range-check the raw major/minor fields, then scale to °C and SG.
// Bounds are applied to the RAW values before scaling (see Config.h), so a
// corrupt frame is rejected before it can reach the alarm and control logic.
// Returns false — and logs why — if the frame fails any check.
static bool decodeTiltReading(const char* fields, int colour,
                              float* sgOut, float* tempCOut, bool* isProOut) {
  uint16_t major, minor;
  if (!hexToU16(fields, &major) || !hexToU16(fields + 4, &minor)) {
    logMsg("[TILT] %s: rejected - non-hex major/minor in frame: %.8s",
      getTiltColourName(colour), fields);
    return false;
  }

  bool isPro = (minor > TILT_PRO_THRESHOLD);
  uint16_t majorMin = isPro ? TILT_PRO_MAJOR_MIN : TILT_MAJOR_MIN;
  uint16_t majorMax = isPro ? TILT_PRO_MAJOR_MAX : TILT_MAJOR_MAX;
  uint16_t minorMin = isPro ? TILT_PRO_MINOR_MIN : TILT_MINOR_MIN;
  uint16_t minorMax = isPro ? TILT_PRO_MINOR_MAX : TILT_MINOR_MAX;

  if (major < majorMin || major > majorMax || minor < minorMin || minor > minorMax) {
    logMsg("[TILT] %s: rejected - %s reading out of range (major=%u minor=%u)",
      getTiltColourName(colour), isPro ? "Pro" : "std", major, minor);
    return false;
  }

  *isProOut  = isPro;
  *tempCOut  = isPro ? ((float)major / 10.0f - 32.0f) * 5.0f / 9.0f
                     : ((float)major - 32.0f) * 5.0f / 9.0f;
  *sgOut     = isPro ? (float)minor / 10000.0f
                     : (float)minor / 1000.0f;
  logMsg("[TILT] PARSE (%s): colour=%d tempF=%u sgRaw=%u tempC=%.1f sg=%.4f",
    isPro ? "Pro" : "std", colour, major, minor, *tempCOut, *sgOut);
  return true;
}

void initBLE() {
  // Hold a full AT+DISI? response (<=320 B, see BLE_BUF_SIZE) even if a loop
  // pass stalls mid-scan. isrBufCapacity is passed EXPLICITLY: left at 0 the
  // library derives it as bufCapacity*10 uint32 entries, so bufCapacity=384
  // with default ISR sizing would cost ~15 KB; capping the edge buffer keeps
  // the total near ~2.4 KB (384 B byte queue + 512*4 B edge queue).
  g_bleSerial.begin(g_globalConfig.bleBaud, SWSERIAL_8N1, PIN_BLE_RX, PIN_BLE_TX,
                    false, /*bufCapacity=*/384, /*isrBufCapacity=*/512);
  delay(100);

  // Send AT to test module responsiveness
  g_bleSerial.print("AT");
  delay(200);

  char resp[32];
  int rLen = 0;
  while (g_bleSerial.available() && rLen < (int)sizeof(resp) - 1) {
    resp[rLen++] = (char)g_bleSerial.read();
  }
  resp[rLen] = '\0';

  if (strstr(resp, "OK") != nullptr) {
    s_bleReady = true;
    logMsg("[BLE] HM-10 module ready");


    g_bleSerial.print("AT+MODE0");   // Transmission mode — required for AT+DISI? scanning
    delay(200);
    while (g_bleSerial.available()) g_bleSerial.read();

    // Set as central role for scanning (AT+ROLE1 on some firmware)
    // and enable iBeacon discovery
    g_bleSerial.print("AT+ROLE1");
    delay(200);
    while (g_bleSerial.available()) g_bleSerial.read();

    g_bleSerial.print("AT+IMME1");  // Don't auto-connect
    delay(200);
    while (g_bleSerial.available()) g_bleSerial.read();

    g_bleSerial.print("AT+RESET");   // Apply all settings
    delay(1000);
    while (g_bleSerial.available()) g_bleSerial.read();
  } else {
    s_bleReady = false;
    logMsg("[BLE] HM-10 module not responding (check wiring D6/D7)");
  }
}

// Parse a single DISC response record for Tilt iBeacon data.
// Handles two formats:
//   Colon-delimited (newer firmware): OK+DISC:CompanyID:UUID:MajorMinorPower:MAC:RSSI
//   Legacy concatenated (older firmware): ...4C000215A495BBx0...{major}{minor}{rssi}
static void parseDiscLine(const char* line) {
  if (!line || strlen(line) < 16) return;  // too short to be a valid DISC record
  logMsg("[TILT] Parsing: %.80s", line);

  // ---- Colon-delimited format ----
  // OK+DISC:004C0215:A495BBx0...(32 chars):MajorMinorPower(10 chars):MAC:RSSI
  const char* p = strstr(line, "OK+DISC:");
  if (p) {
    p += 8;  // skip "OK+DISC:"
    // Field 1: CompanyID — must be Apple iBeacon prefix
    if (strncasecmp(p, "004C0215", 8) == 0) {
      p += 8;
      if (*p == ':') p++;
      // Field 2: UUID (32 hex chars) — must match the full Tilt UUID
      int colour = identifyTiltUuid(p);
      if (colour < 0) {
        logMsg("[TILT] Apple iBeacon found but not a Tilt UUID: %.32s", p);
        return;
      }
      p += 32;
      if (*p == ':') p++;
      // Field 3: MajorMinorPower — 4 Major + 4 Minor + 2 Power = 10 chars
      if (strlen(p) >= 8) {
        float sg, tempC;
        bool  isPro;
        if (decodeTiltReading(p, colour, &sg, &tempC, &isPro)) {
          processTiltReading(colour, sg, tempC, isPro);
        }
      }
      return;
    }
  }

  // ---- Legacy concatenated format ----
  // ...4C000215A495BBx0{32-char-uuid}{4-major}{4-minor}{2-rssi}...
  const char* dataStart = strstr(line, "4C000215");
  if (!dataStart) return;
  if (strlen(dataStart) < 48) return;

  // UUID starts 8 chars in, after the "4C000215" company/type prefix
  int colour = identifyTiltUuid(dataStart + 8);
  if (colour < 0) {
    logMsg("[TILT] iBeacon found (legacy fmt) but not a Tilt: %.16s...", dataStart + 8);
    return;
  }
  float sg, tempC;
  bool  isPro;
  if (decodeTiltReading(dataStart + 40, colour, &sg, &tempC, &isPro)) {
    processTiltReading(colour, sg, tempC, isPro);
  }
}

// Kick off an AT+DISI? scan on the 5 s tick if idle and allowed. serviceTilt()
// (called every loop pass) drains the response — this function never blocks on
// serial reads.
void startTiltScan() {
  // Skip Tilt scanning when BLE sniff page owns the serial port
  if (g_bleSniffActive) return;
  // Previous scan still draining (4 s window < 5 s tick, so rare)
  if (s_scanActive) return;
  // Interlock: a queued cloud POST may run this window and block the loop up to
  // 5 s — skip this tick rather than start a scan that would be starved. Tilt
  // advertises continuously, so the next tick (5 s) picks it up.
  if (reportsPending()) return;

  if (!s_bleReady) {
    // One failed boot AT probe used to disable Tilt until reboot; retry the
    // init at a bounded 5-min cadence (initBLE blocks ~1.9 s).
    unsigned long now = millis();
    if (now - s_lastBleInitRetry >= BLE_REINIT_RETRY_MS) {
      s_lastBleInitRetry = now;
      logMsg("[BLE] Retrying HM-10 init...");
      initBLE();
    }
    if (!s_bleReady) {
      // Still increment missed reads for timeout
      for (int i = 0; i < MAX_TILTS; i++) {
        if (g_tilts[i].active) {
          s_missedReads[i]++;
          if (s_missedReads[i] >= 300) {
            logMsg("[TILT] %s: Tilt not seen in 300 attempted reads. Deregistering.",
              getTiltColourName(i));
            g_tilts[i].active = false;
            s_missedReads[i] = 0;
          }
        }
      }
      return;
    }
  }

  logMsg("[TILT] Starting BLE discovery scan.");

  // Start iBeacon discovery scan; serviceTilt() drains the response.
  g_bleSerial.print("AT+DISI?");
  s_scanStart  = millis();
  s_bleBufLen  = 0;
  s_scanActive = true;
}

// Drain an in-flight AT+DISI? response, a chunk per loop pass (no busy-wait).
// Records are parsed as soon as a second OK+DISC: delimiter is seen in the
// buffer, so the working buffer only ever holds ~2 records at a time regardless
// of how many devices are in range.  This prevents overflow losing the Tilt packet.
void serviceTilt() {
  if (!s_scanActive) return;

  // Sniff page opened mid-scan: abandon the scan so its direct serial reads
  // don't fight this drain loop.
  if (g_bleSniffActive) {
    s_scanActive = false;
    s_bleBufLen  = 0;
    return;
  }

  bool scanDone = false;
  while (g_bleSerial.available()) {
    char c = (char)g_bleSerial.read();

    // Prevent buffer overflow — should not normally be needed with the record-flush
    // logic below, but kept as a safety net for malformed/unexpected responses.
    if (s_bleBufLen >= BLE_BUF_SIZE - 1) {
      int half = BLE_BUF_SIZE / 2;
      memmove(s_bleBuf, s_bleBuf + half, s_bleBufLen - half);
      s_bleBufLen -= half;
    }
    s_bleBuf[s_bleBufLen++] = c;
    s_bleBuf[s_bleBufLen] = '\0';

    // Check for end-of-scan marker
    if (s_bleBufLen >= 8 && strcmp(s_bleBuf + s_bleBufLen - 8, "OK+DISCE") == 0) {
      // Strip the marker so the last record is cleanly processed below
      s_bleBufLen -= 8;
      s_bleBuf[s_bleBufLen] = '\0';
      scanDone = true;
      break;
    }

    // When a second OK+DISC: appears in the buffer, the first record is complete.
    // Save the char at the boundary, null-terminate, parse, then RESTORE before
    // memmove — otherwise the 'O' of the next "OK+DISC:" is clobbered and lost.
    char* second = strstr(s_bleBuf + 8, "OK+DISC:");
    if (second) {
      char saved = *second;
      *second = '\0';
      // Only parse actual DISC records — ignore scan start markers (OK+DISCS etc.)
      if (strncmp(s_bleBuf, "OK+DISC:", 8) == 0) {
        parseDiscLine(s_bleBuf);
      }
      *second = saved;  // restore 'O' before memmove
      int firstLen = (int)(second - s_bleBuf);
      int remain   = s_bleBufLen - firstLen;
      memmove(s_bleBuf, second, remain + 1);
      s_bleBufLen = remain;
    }
  }

  // Not finished and still within the scan window — resume draining next pass.
  if (!scanDone && millis() - s_scanStart < BLE_SCAN_TIMEOUT_MS) return;

  // Scan over (end marker or timeout). Parse any record(s) left in the buffer.
  const char* pos = s_bleBuf;
  while (pos < s_bleBuf + s_bleBufLen) {
    const char* found = strstr(pos, "OK+DISC:");
    if (!found) break;
    parseDiscLine(found);
    pos = found + 8;
  }
  s_bleBufLen  = 0;
  s_scanActive = false;

  // Increment missed reads for active Tilts not seen this scan
  for (int i = 0; i < MAX_TILTS; i++) {
    if (g_tilts[i].active) {
      s_missedReads[i]++;
      if (s_missedReads[i] == 100) {
        logMsg("[TILT] %s: Tilt not seen in 100 attempted reads",
          getTiltColourName(i));
      } else if (s_missedReads[i] == 200) {
        logMsg("[TILT] %s: Tilt not seen in 200 attempted reads",
          getTiltColourName(i));
      } else if (s_missedReads[i] >= 300) {
        logMsg("[TILT] %s: Tilt not seen in 300 attempted reads. Deregistering.",
          getTiltColourName(i));
        g_tilts[i].active = false;
        s_missedReads[i] = 0;
      }
    }
  }
}

void processTiltReading(uint8_t colour, float sg, float tempC, bool isPro) {
  if (colour >= MAX_TILTS) return;

  // Mark colour so saveTiltConfig() includes this slot (first time seen)
  if (g_tilts[colour].colour == PROBE_UNASSIGNED) {
    g_tilts[colour].colour = colour;
  }

  // Apply calibration offsets from stored config
  g_tilts[colour].sg          = sg    + g_tilts[colour].sgAdjust;
  g_tilts[colour].temperature = tempC + g_tilts[colour].tempAdjust;
  g_tilts[colour].active      = true;
  g_tilts[colour].lastSeen    = millis();
  g_tilts[colour].isPro       = isPro;
  s_missedReads[colour]       = 0;

  logMsg("[TILT] %s: SG=%.4f (raw %.4f) T=%.1fC (raw %.1f)",
    getTiltColourName(colour),
    g_tilts[colour].sg, sg,
    g_tilts[colour].temperature, tempC);
}

const char* getTiltUUID(uint8_t colour) {
  if (colour >= MAX_TILTS) return "";
  return TILT_UUIDS[colour];
}

const char* getTiltColourName(uint8_t colour) {
  if (colour >= MAX_TILTS) return "Unknown";
  return TILT_COLOUR_NAMES[colour];
}
