/*
 * Webhook.cpp — Template engine + tag→category mapping
 *
 * The dispatcher (added in a follow-up commit) drives this; here we provide
 * the pure-function pieces it depends on.
 */

#include "Webhook.h"
#include "Temperatures.h"
#include <string.h>
#include <stdio.h>

// ============================================================
// Tag → category mapping
// ============================================================

uint32_t tagToCategory(const char* msg) {
  if (!msg) return 0;
  while (*msg == ' ') msg++;
  if (*msg != '[') return 0;
  msg++;

  char tag[16];
  size_t len = 0;
  while (len < sizeof(tag) - 1 && *msg && *msg != ']') {
    tag[len++] = *msg++;
  }
  tag[len] = '\0';

  if      (!strcmp(tag, "ALARM"))  return WEBHOOK_CAT_ALARM;
  else if (!strcmp(tag, "TEMP"))   return WEBHOOK_CAT_ALARM;  // probe failures grouped with alarms
  else if (!strcmp(tag, "FERM"))   return WEBHOOK_CAT_FERM;
  else if (!strcmp(tag, "SYS"))    return WEBHOOK_CAT_SYS;
  else if (!strcmp(tag, "WIFI"))   return WEBHOOK_CAT_WIFI;
  else if (!strcmp(tag, "PLUG"))   return WEBHOOK_CAT_PLUG;
  else if (!strcmp(tag, "CFG"))    return WEBHOOK_CAT_CFG;
  else if (!strcmp(tag, "OTA"))    return WEBHOOK_CAT_OTA;
  else if (!strcmp(tag, "PROF"))   return WEBHOOK_CAT_PROF;
  return 0;
}

// ============================================================
// Encoding helpers
// ============================================================

enum EncMode { ENC_RAW, ENC_JSON, ENC_URL };

static int appendJsonEscaped(const char* src, char* out, size_t outLen, size_t* pos) {
  if (!src) return 0;
  while (*src) {
    char c = *src++;
    const char* esc = nullptr;
    char escBuf[8];
    switch (c) {
      case '"':  esc = "\\\""; break;
      case '\\': esc = "\\\\"; break;
      case '\n': esc = "\\n";  break;
      case '\r': esc = "\\r";  break;
      case '\t': esc = "\\t";  break;
      case '\b': esc = "\\b";  break;
      case '\f': esc = "\\f";  break;
      default:
        if ((unsigned char)c < 0x20) {
          snprintf(escBuf, sizeof(escBuf), "\\u%04x", (unsigned char)c);
          esc = escBuf;
        }
        break;
    }
    if (esc) {
      size_t l = strlen(esc);
      if (*pos + l + 1 > outLen) return -1;
      memcpy(out + *pos, esc, l);
      *pos += l;
    } else {
      if (*pos + 2 > outLen) return -1;
      out[(*pos)++] = c;
    }
  }
  return 0;
}

static int appendUrlEncoded(const char* src, char* out, size_t outLen, size_t* pos) {
  if (!src) return 0;
  static const char hex[] = "0123456789ABCDEF";
  while (*src) {
    unsigned char c = (unsigned char)*src++;
    bool unreserved =
      (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z') ||
      (c >= '0' && c <= '9') ||
      c == '-' || c == '_' || c == '.' || c == '~';
    if (unreserved) {
      if (*pos + 2 > outLen) return -1;
      out[(*pos)++] = (char)c;
    } else {
      if (*pos + 4 > outLen) return -1;
      out[(*pos)++] = '%';
      out[(*pos)++] = hex[c >> 4];
      out[(*pos)++] = hex[c & 0x0F];
    }
  }
  return 0;
}

static int appendRaw(const char* src, char* out, size_t outLen, size_t* pos) {
  if (!src) return 0;
  size_t l = strlen(src);
  if (*pos + l + 1 > outLen) return -1;
  memcpy(out + *pos, src, l);
  *pos += l;
  return 0;
}

static int appendEncoded(const char* src, EncMode enc, char* out, size_t outLen, size_t* pos) {
  switch (enc) {
    case ENC_JSON: return appendJsonEscaped(src, out, outLen, pos);
    case ENC_URL:  return appendUrlEncoded(src, out, outLen, pos);
    default:       return appendRaw(src, out, outLen, pos);
  }
}

// ============================================================
// Variable resolution
// ============================================================

static const char* levelName(uint8_t level) {
  switch (level) {
    case 0: return "EMERG";
    case 1: return "ALERT";
    case 2: return "CRIT";
    case 3: return "ERR";
    case 4: return "WARNING";
    case 5: return "NOTICE";
    case 6: return "INFO";
    case 7: return "DEBUG";
    default: return "?";
  }
}

// Resolve `name` to a string. Numeric values are formatted into `valBuf`;
// string values point into existing storage. Returns true on match.
static bool resolveVar(const char* name, const WebhookEvent& evt,
                       char* valBuf, size_t valBufLen,
                       const char** valPtr) {
  if (!strcmp(name, "MSG"))   { *valPtr = evt.msg; return true; }
  if (!strcmp(name, "TAG"))   { *valPtr = evt.tag; return true; }
  if (!strcmp(name, "LEVEL")) { *valPtr = levelName(evt.level); return true; }
  if (!strcmp(name, "TS"))    { snprintf(valBuf, valBufLen, "%lu", (unsigned long)evt.ts); *valPtr = valBuf; return true; }
  if (!strcmp(name, "DEVICE")) {
    static char devName[24] = {0};
    if (!devName[0]) snprintf(devName, sizeof(devName), "ourbrewbot-%06x", ESP.getChipId() & 0xFFFFFF);
    *valPtr = devName;
    return true;
  }
  if (!strcmp(name, "FERM_INDEX")) {
    if (evt.fermIndex >= MAX_FERMENTERS) *valPtr = "";
    else { snprintf(valBuf, valBufLen, "%u", (unsigned)evt.fermIndex); *valPtr = valBuf; }
    return true;
  }
  if (!strcmp(name, "FERM_NAME")) {
    *valPtr = (evt.fermIndex >= MAX_FERMENTERS) ? "" : g_fermenters[evt.fermIndex].fermenterName;
    return true;
  }
  if (!strcmp(name, "FERM_TEMP")) {
    if (evt.fermIndex >= MAX_FERMENTERS) { *valPtr = ""; return true; }
    float t = getBeerTemp(evt.fermIndex);
    if (t < -100.0f) *valPtr = "";
    else { snprintf(valBuf, valBufLen, "%.1f", t); *valPtr = valBuf; }
    return true;
  }
  if (!strcmp(name, "FERM_TARGET")) {
    if (evt.fermIndex >= MAX_FERMENTERS) { *valPtr = ""; return true; }
    float target = (g_fermenters[evt.fermIndex].floorTemp + g_fermenters[evt.fermIndex].ceilingTemp) / 2.0f;
    snprintf(valBuf, valBufLen, "%.1f", target);
    *valPtr = valBuf;
    return true;
  }
  return false;
}

// ============================================================
// renderTemplate
// ============================================================

int renderTemplate(const char* tmpl, const WebhookEvent& evt, char* out, size_t outLen) {
  if (!tmpl || !out || outLen == 0) return -1;
  size_t pos = 0;

  while (*tmpl) {
    if (*tmpl != '$') {
      if (pos + 2 > outLen) return -1;
      out[pos++] = *tmpl++;
      continue;
    }

    // Scan identifier chars after $
    const char* idStart = tmpl + 1;
    const char* idEnd = idStart;
    while ((*idEnd >= 'A' && *idEnd <= 'Z') ||
           (*idEnd >= '0' && *idEnd <= '9') ||
           *idEnd == '_') {
      idEnd++;
    }
    size_t idLen = idEnd - idStart;
    if (idLen == 0) {
      // Lone $ — emit literally
      if (pos + 2 > outLen) return -1;
      out[pos++] = *tmpl++;
      continue;
    }

    char ident[40];
    if (idLen >= sizeof(ident)) {
      // Unknown long identifier — emit literally
      if (pos + 1 + idLen + 1 > outLen) return -1;
      out[pos++] = '$';
      memcpy(out + pos, idStart, idLen);
      pos += idLen;
      tmpl = idEnd;
      continue;
    }
    memcpy(ident, idStart, idLen);
    ident[idLen] = '\0';

    // Strip JSON_ / URL_ prefix to pick encoding
    EncMode enc = ENC_RAW;
    const char* varName = ident;
    if (idLen > 5 && !strncmp(ident, "JSON_", 5))      { enc = ENC_JSON; varName = ident + 5; }
    else if (idLen > 4 && !strncmp(ident, "URL_", 4))  { enc = ENC_URL;  varName = ident + 4; }

    char valBuf[32];
    const char* val = nullptr;
    if (resolveVar(varName, evt, valBuf, sizeof(valBuf), &val)) {
      if (appendEncoded(val, enc, out, outLen, &pos) < 0) return -1;
    } else {
      // Unrecognised variable — emit literal `$XYZ`
      if (pos + 1 + idLen + 1 > outLen) return -1;
      out[pos++] = '$';
      memcpy(out + pos, idStart, idLen);
      pos += idLen;
    }
    tmpl = idEnd;
  }

  if (pos >= outLen) return -1;
  out[pos] = '\0';
  return (int)pos;
}
