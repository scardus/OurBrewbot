/*
 * Tilt.cpp — Tilt hydrometer support via HM-10 BLE module
 *
 * KeyeStudio Bluetooth 4.0 v2 (HM-10 / CC2541 compatible)
 * communicates via SoftwareSerial AT commands.
 *
 * AT+DISI? returns iBeacon advertisements in format:
 *   OK+DISC:00000000:00000000:4C000215A495BB10C5B14B44B5121370F02D74DE00E703F0C5
 *   Where: 4C000215 = iBeacon prefix
 *          A495BBx0...74DE = Tilt UUID (x = colour: 1=Red..8=Pink)
 *          00E7 = Major (temp °F)
 *          03F0 = Minor (SG × 10000)
 *          C5 = measured power (RSSI)
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

    // Set as central role for scanning (AT+ROLE1 on some firmware)
    // and enable iBeacon discovery
    g_bleSerial.print("AT+ROLE1");
    delay(200);
    while (g_bleSerial.available()) g_bleSerial.read();  // flush

    g_bleSerial.print("AT+IMME1");  // Don't auto-connect
    delay(200);
    while (g_bleSerial.available()) g_bleSerial.read();
  } else {
    s_bleReady = false;
    logMsg("[BLE] HM-10 module not responding (check wiring D6/D7)");
  }
}

// Parse a single DISC response line for Tilt iBeacon data
static void parseDiscLine(const char* line) {
  // Format: OK+DISC:DevAddr:Reserved:iBeaconData
  // iBeaconData = 4C000215 + 32-char UUID + 4-char Major + 4-char Minor + 2-char RSSI
  // Total iBeacon hex = 8 + 32 + 4 + 4 + 2 = 50 chars

  const char* dataStart = strstr(line, "4C000215");
  if (!dataStart) return;

  // Need at least 48 chars from the start of iBeacon data (8+32+4+4)
  if (strlen(dataStart) < 48) return;

  const char* data = dataStart;

  int colour = identifyTiltColour(data);
  if (colour < 0) return;

  // Major = temp in °F (4 hex chars after 8+32 = 40 offset)
  uint16_t tempF = hexToU16(data + 40);
  // Minor = SG × 10000 (4 hex chars after 44 offset)
  uint16_t sgRaw = hexToU16(data + 44);

  float tempC = ((float)tempF - 32.0f) * 5.0f / 9.0f;
  float sg = (float)sgRaw / 10000.0f;

  processTiltReading(colour, sg, tempC);
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

  // Start iBeacon discovery scan
  g_bleSerial.print("AT+DISI?");

  // Read responses with timeout (scan takes ~2-3 seconds)
  unsigned long start = millis();
  s_bleBufLen = 0;
  bool scanDone = false;

  while (millis() - start < 4000 && !scanDone) {
    while (g_bleSerial.available()) {
      char c = (char)g_bleSerial.read();

      // Prevent buffer overflow — discard oldest data if full
      if (s_bleBufLen >= BLE_BUF_SIZE - 1) {
        // Shift buffer left by half to make room
        int half = BLE_BUF_SIZE / 2;
        memmove(s_bleBuf, s_bleBuf + half, s_bleBufLen - half);
        s_bleBufLen -= half;
      }
      s_bleBuf[s_bleBufLen++] = c;
      s_bleBuf[s_bleBufLen] = '\0';

      // Check for end-of-scan marker
      if (s_bleBufLen >= 8 && strcmp(s_bleBuf + s_bleBufLen - 8, "OK+DISCE") == 0) {
        scanDone = true;
        break;
      }

      // Process complete lines as we receive them
      char* nl = strchr(s_bleBuf, '\n');
      if (nl) {
        *nl = '\0';
        // Parse the line in-place (s_bleBuf up to nl)
        if (s_bleBuf[0] != '\0') {
          parseDiscLine(s_bleBuf);
        }
        // Shift remainder forward
        int remain = s_bleBufLen - (int)(nl - s_bleBuf) - 1;
        if (remain > 0) {
          memmove(s_bleBuf, nl + 1, remain);
        }
        s_bleBufLen = remain > 0 ? remain : 0;
        s_bleBuf[s_bleBufLen] = '\0';
      }
    }
    yield();
  }

  // Process any remaining data in buffer
  if (s_bleBufLen > 0) {
    // Scan buffer for any iBeacon data markers
    const char* pos = s_bleBuf;
    while (pos < s_bleBuf + s_bleBufLen) {
      const char* found = strstr(pos, "4C000215");
      if (!found) break;
      parseDiscLine(found);
      pos = found + 48;
    }
    s_bleBufLen = 0;
  }

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

void processTiltReading(uint8_t colour, float sg, float tempC) {
  if (colour >= MAX_TILTS) return;
  g_tilts[colour].sg          = sg;
  g_tilts[colour].temperature = tempC;
  g_tilts[colour].active      = true;
  g_tilts[colour].lastSeen    = millis();
  s_missedReads[colour]       = 0;

  logMsg("[TILT] %s: SG=%.4f T=%.1fC",
    getTiltColourName(colour), sg, tempC);
}

const char* getTiltUUID(uint8_t colour) {
  if (colour >= MAX_TILTS) return "";
  return TILT_UUIDS[colour];
}

const char* getTiltColourName(uint8_t colour) {
  if (colour >= MAX_TILTS) return "Unknown";
  return TILT_COLOUR_NAMES[colour];
}
