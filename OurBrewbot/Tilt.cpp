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

// Parse a 4-char hex string to uint16_t
static uint16_t hexToU16(const char* hex) {
  uint16_t val = 0;
  for (int i = 0; i < 4; i++) {
    val <<= 4;
    char c = hex[i];
    if (c >= '0' && c <= '9') val |= (c - '0');
    else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
    else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
  }
  return val;
}

// Try to identify a Tilt colour from the iBeacon UUID hex string
// Returns colour index (0-7) or -1 if not a Tilt
static int identifyTiltColour(const char* uuidHex) {
  // Tilt UUID pattern: 4C000215 A495BB x0 C5B14B44B5121370F02D74DE
  // Check for "A495BB" prefix at expected position (after 4C000215 = 8 chars)
  if (strncasecmp(uuidHex + 8, "A495BB", 6) != 0) return -1;

  char colourByte = uuidHex[14];  // The distinguishing digit
  for (int i = 0; i < MAX_TILTS; i++) {
    if (colourByte == TILT_UUID_BYTES[i]) return i;
  }
  return -1;
}

void initBLE() {
  g_bleSerial.begin(g_globalConfig.bleBaud);
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
      // Field 2: UUID (32 hex chars) — check for Tilt A495BBx0 pattern
      if (strlen(p) >= 32 && strncasecmp(p, "A495BB", 6) == 0) {
        char colourChar = p[6];
        int colour = -1;
        for (int i = 0; i < MAX_TILTS; i++) {
          if (colourChar == TILT_UUID_BYTES[i]) { colour = i; break; }
        }
        if (colour < 0) {
          logMsg("[TILT] Tilt-like UUID but unknown colour byte: %c", colourChar);
          return;
        }
        p += 32;
        if (*p == ':') p++;
        // Field 3: MajorMinorPower — 4 Major + 4 Minor + 2 Power = 10 chars
        if (strlen(p) >= 8) {
          uint16_t tempF = hexToU16(p);      // Major = temp in °F (× 10 for Pro)
          uint16_t sgRaw = hexToU16(p + 4);  // Minor = SG × 1000 (standard) or × 10000 (Pro)
          bool isPro = (tempF > 212);         // Pro reports tenths; 212°F = boiling, impossible in use
          float tempC = isPro ? ((float)tempF / 10.0f - 32.0f) * 5.0f / 9.0f
                              : ((float)tempF - 32.0f) * 5.0f / 9.0f;
          float sg    = isPro ? (float)sgRaw / 10000.0f
                              : (float)sgRaw / 1000.0f;
          logMsg("[TILT] PARSE (%s): colour=%d tempF=%u sgRaw=%u tempC=%.1f sg=%.4f",
            isPro ? "Pro" : "std", colour, tempF, sgRaw, tempC, sg);
          processTiltReading(colour, sg, tempC, isPro);
          return;
        }
      } else {
        logMsg("[TILT] Apple iBeacon found but not a Tilt UUID: %.32s", p);
        return;
      }
    }
  }

  // ---- Legacy concatenated format ----
  // ...4C000215A495BBx0{32-char-uuid}{4-major}{4-minor}{2-rssi}...
  const char* dataStart = strstr(line, "4C000215");
  if (!dataStart) return;
  if (strlen(dataStart) < 48) return;

  int colour = identifyTiltColour(dataStart);
  if (colour < 0) {
    logMsg("[TILT] iBeacon found (legacy fmt) but not a Tilt: %.16s...", dataStart + 8);
    return;
  }
  uint16_t tempF = hexToU16(dataStart + 40);  // Major = temp in °F (× 10 for Pro)
  uint16_t sgRaw = hexToU16(dataStart + 44);  // Minor = SG × 1000 (standard) or × 10000 (Pro)
  bool isPro = (tempF > 212);
  float tempC = isPro ? ((float)tempF / 10.0f - 32.0f) * 5.0f / 9.0f
                      : ((float)tempF - 32.0f) * 5.0f / 9.0f;
  float sg    = isPro ? (float)sgRaw / 10000.0f
                      : (float)sgRaw / 1000.0f;
  logMsg("[TILT] PARSE (legacy/%s): colour=%d tempF=%u sgRaw=%u tempC=%.1f sg=%.4f",
    isPro ? "Pro" : "std", colour, tempF, sgRaw, tempC, sg);
  processTiltReading(colour, sg, tempC, isPro);
}

void checkTilt() {
  // Skip Tilt scanning when BLE sniff page is active
  if (g_bleSniffActive) return;

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

  logMsg("[TILT] Starting BLE discovery scan.");

  // Start iBeacon discovery scan
  g_bleSerial.print("AT+DISI?");

  // Read responses with timeout (scan takes ~2-3 seconds).
  // Records are parsed as soon as a second OK+DISC: delimiter is seen in the buffer,
  // so the working buffer only ever holds ~2 records at a time regardless of how many
  // devices are in range.  This prevents overflow losing the Tilt packet.
  unsigned long start = millis();
  s_bleBufLen = 0;
  bool scanDone = false;

  while (millis() - start < 4000 && !scanDone) {
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
    yield();
  }

  // Parse any record(s) remaining in the buffer after the scan ends
  const char* pos = s_bleBuf;
  while (pos < s_bleBuf + s_bleBufLen) {
    const char* found = strstr(pos, "OK+DISC:");
    if (!found) break;
    parseDiscLine(found);
    pos = found + 8;
  }
  s_bleBufLen = 0;

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
