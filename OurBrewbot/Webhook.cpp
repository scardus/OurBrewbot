/*
 * Webhook.cpp — Template engine + tag→category mapping
 *
 * The dispatcher (added in a follow-up commit) drives this; here we provide
 * the pure-function pieces it depends on.
 */

#include "Webhook.h"
#include "Temperatures.h"
#include "Https.h"
#include "Log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <umm_malloc/umm_heap_select.h>

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

// ============================================================
// Dispatcher queue (ring buffer in IRAM heap)
// ============================================================

static const size_t QUEUE_SIZE      = 8;
static const size_t RENDER_BUF_SIZE = 512;

static WebhookEvent* s_queue     = nullptr;
static char*         s_renderBuf = nullptr;
static size_t        s_head      = 0;
static size_t        s_tail      = 0;
static size_t        s_count     = 0;

void webhookInit() {
  if (s_queue && s_renderBuf) return;
  {
    HeapSelectIram ephemeral;
    if (!s_queue)     s_queue     = (WebhookEvent*)calloc(QUEUE_SIZE, sizeof(WebhookEvent));
    if (!s_renderBuf) s_renderBuf = (char*)malloc(RENDER_BUF_SIZE);
  }
  if (!s_queue || !s_renderBuf) {
    logMsgL(SYSLOG_WARNING, "[HOOK] IRAM alloc failed; webhooks disabled this boot");
  }
}

// Extract the [TAG] from msg into `out` (without brackets).
static void extractTag(const char* msg, char* out, size_t outLen) {
  out[0] = '\0';
  while (*msg == ' ') msg++;
  if (*msg != '[') return;
  msg++;
  size_t n = 0;
  while (n < outLen - 1 && *msg && *msg != ']') {
    out[n++] = *msg++;
  }
  out[n] = '\0';
}

// Best-effort: look for " F<digits>" pattern in tagged messages (e.g.
// "[ALARM] F0 (Fermenter 1)..."). Returns MAX_FERMENTERS if not found.
static uint8_t extractFermIndex(const char* msg) {
  if (!msg) return MAX_FERMENTERS;
  const char* p = strstr(msg, " F");
  if (!p) return MAX_FERMENTERS;
  p += 2;
  if (*p < '0' || *p > '9') return MAX_FERMENTERS;
  uint8_t idx = (uint8_t)(*p - '0');
  p++;
  if (*p >= '0' && *p <= '9') idx = (uint8_t)(idx * 10 + (*p - '0'));
  if (idx >= MAX_FERMENTERS) return MAX_FERMENTERS;
  return idx;
}

void webhookEnqueue(uint8_t level, const char* msg) {
  if (!s_queue || !msg) return;

  uint32_t category = tagToCategory(msg);
  if (category == 0) return;

  uint8_t fermIndex = extractFermIndex(msg);

  // Compute which slots want this event
  uint8_t mask = 0;
  for (uint8_t i = 0; i < MAX_WEBHOOKS; i++) {
    if (!g_webhooks[i].enabled) continue;
    if (g_webhooks[i].url[0] == '\0') continue;
    if (level > g_webhooks[i].minLevel) continue;     // syslog convention: lower = more critical
    if (!(g_webhooks[i].eventMask & category)) continue;
    mask |= (uint8_t)(1u << i);
  }
  if (mask == 0) return;

  // Drop oldest on overflow (lossy). Do NOT logMsg from here — would recurse.
  if (s_count == QUEUE_SIZE) {
    s_head = (s_head + 1) % QUEUE_SIZE;
    s_count--;
  }

  WebhookEvent& evt = s_queue[s_tail];
  evt.level          = level;
  evt.ts             = millis();
  evt.fermIndex      = fermIndex;
  evt.category       = category;
  evt.subscriberMask = mask;
  extractTag(msg, evt.tag, sizeof(evt.tag));
  strlcpy(evt.msg, msg, sizeof(evt.msg));

  s_tail = (s_tail + 1) % QUEUE_SIZE;
  s_count++;
}

static const char* methodName(uint8_t m) {
  switch (m) {
    case WEBHOOK_METHOD_GET: return "GET";
    case WEBHOOK_METHOD_PUT: return "PUT";
    default:                 return "POST";
  }
}

// Deliver one slot from one event. Updates the slot's lastFireMs/lastHttpCode
// and clears its bit from the subscriber mask. Returns HTTP code (or negative).
// URL, body template, and auth header are all run through renderTemplate so
// $VARS work in any of them (e.g. ntfy's "Title: OurBrewbot $TAG" header).
static int deliverOne(uint8_t slotIndex, WebhookEvent& evt) {
  WebhookConfig& s = g_webhooks[slotIndex];

  char urlBuf[200];
  char authBuf[200];

  if (renderTemplate(s.url, evt, urlBuf, sizeof(urlBuf)) < 0) {
    logMsgL(SYSLOG_WARNING, "[HOOK] Slot %u: URL render overflow", slotIndex);
    evt.subscriberMask &= (uint8_t)~(1u << slotIndex);
    return -100;
  }
  if (renderTemplate(s.authHeader, evt, authBuf, sizeof(authBuf)) < 0) {
    logMsgL(SYSLOG_WARNING, "[HOOK] Slot %u: auth header render overflow", slotIndex);
    evt.subscriberMask &= (uint8_t)~(1u << slotIndex);
    return -100;
  }
  if (renderTemplate(s.bodyTemplate, evt, s_renderBuf, RENDER_BUF_SIZE) < 0) {
    logMsgL(SYSLOG_WARNING, "[HOOK] Slot %u: body render overflow", slotIndex);
    evt.subscriberMask &= (uint8_t)~(1u << slotIndex);
    return -100;
  }

  int code = httpsRequest(methodName(s.method),
                          urlBuf,
                          s.contentType,
                          s_renderBuf,
                          authBuf,
                          10000,
                          nullptr);

  s.lastFireMs   = millis();
  s.lastHttpCode = (uint16_t)((code < 0 || code > 0xFFFF) ? 0xFFFF : code);
  evt.subscriberMask &= (uint8_t)~(1u << slotIndex);

  logMsg("[HOOK] Slot %u %s %s -> %d", slotIndex, methodName(s.method), urlBuf, code);
  return code;
}

void webhookLoop() {
  if (s_count == 0 || !s_queue || !s_renderBuf) return;

  WebhookEvent& evt = s_queue[s_head];

  // Find first subscribed slot that isn't currently rate-limited
  for (uint8_t i = 0; i < MAX_WEBHOOKS; i++) {
    if (!(evt.subscriberMask & (1u << i))) continue;
    if (!g_webhooks[i].enabled) {
      evt.subscriberMask &= (uint8_t)~(1u << i);
      continue;
    }
    // Rate limit
    if (g_webhooks[i].rateLimitSec > 0 && g_webhooks[i].lastFireMs > 0) {
      uint32_t since = millis() - g_webhooks[i].lastFireMs;
      if (since < (uint32_t)g_webhooks[i].rateLimitSec * 1000) {
        // Drop this slot for this event — don't hold the queue waiting
        evt.subscriberMask &= (uint8_t)~(1u << i);
        continue;
      }
    }

    deliverOne(i, evt);
    // One HTTPS call per loop tick — yield
    if (evt.subscriberMask == 0) {
      s_head = (s_head + 1) % QUEUE_SIZE;
      s_count--;
    }
    return;
  }

  // No remaining subscribers — dequeue
  s_head = (s_head + 1) % QUEUE_SIZE;
  s_count--;
}

int webhookFireTest(uint8_t slotIndex) {
  if (slotIndex >= MAX_WEBHOOKS) return -1;
  if (!s_renderBuf) return -2;
  if (g_webhooks[slotIndex].url[0] == '\0') return -3;

  // Synthetic event: pretend it's an ALARM on fermenter 0
  WebhookEvent evt = {};
  evt.level     = SYSLOG_WARNING;
  evt.ts        = millis();
  evt.fermIndex = 0;
  evt.category  = WEBHOOK_CAT_ALARM;
  evt.subscriberMask = (uint8_t)(1u << slotIndex);
  strlcpy(evt.tag, "ALARM", sizeof(evt.tag));
  strlcpy(evt.msg, "[ALARM] Test event from OurBrewbot", sizeof(evt.msg));

  return deliverOne(slotIndex, evt);
}
