#pragma once
/*
 * Webhook.h — Event-driven webhook delivery
 *
 * The dispatcher receives events (via webhookEnqueue) from logMsgL, computes
 * which configured slots want each event based on severity + category, and
 * fires HTTPS requests in the main loop using rendered body templates.
 *
 * Templates use $VAR substitution with three forms per variable:
 *   $MSG         raw text
 *   $JSON_MSG    escaped for JSON string context
 *   $URL_MSG     percent-encoded for form bodies / URL paths
 *
 * Available variables: MSG, TAG, LEVEL, TS, DEVICE, FERM_INDEX,
 * FERM_NAME, FERM_TEMP, FERM_TARGET. The FERM_* set substitutes empty
 * when the event has no associated fermenter.
 */

#include <Arduino.h>
#include "Config.h"

// One event in the dispatcher queue. Roughly 200 bytes per entry.
struct WebhookEvent {
  uint8_t  level;            // SYSLOG_*
  uint32_t ts;               // millis() when enqueued
  char     tag[16];          // e.g. "ALARM"
  char     msg[160];         // full formatted message (including [TAG] prefix)
  uint8_t  fermIndex;        // 0..MAX_FERMENTERS-1 or 0xFF if N/A
  uint32_t category;         // WEBHOOK_CAT_* bit, or 0 if unmapped tag
  uint8_t  subscriberMask;   // which slots want this (bit per webhook index)
};

// Return the WEBHOOK_CAT_* bit corresponding to the [TAG] at the start of msg,
// or 0 if the tag is unrecognised (in which case no webhook should fire).
uint32_t tagToCategory(const char* msg);

// Render `tmpl` into `out` (size outLen) using `evt` for variable substitution.
// Returns rendered length on success, or -1 on overflow. The output is always
// null-terminated when the function returns >= 0.
int renderTemplate(const char* tmpl, const WebhookEvent& evt, char* out, size_t outLen);

// Allocate the dispatcher queue + render buffer on the IRAM second heap.
// Idempotent. Call once at boot after Config is loaded.
void webhookInit();

// Enqueue an event for delivery. Computes the subscriber mask from configured
// slots; drops silently if no slot wants it (unknown tag, all slots disabled,
// or all slot filters reject the level/category). Drops oldest on overflow.
void webhookEnqueue(uint8_t level, const char* msg, uint8_t fermIndex);

// Drain at most one delivery per call. Intended to run from the main loop;
// each delivery is a blocking HTTPS request (~2-8 s).
void webhookLoop();

// Render + send a single test event to one slot. Returns HTTP status code
// (or negative transport error). Used by the /webhook/test endpoint.
int webhookFireTest(uint8_t slotIndex);
