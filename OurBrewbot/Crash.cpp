/*
 * Crash.cpp — Exception stack capture + main-loop checkpoint breadcrumb.
 *
 * Two RTC slots:
 *   - CrashRecord at offset 0  (filled by custom_crash_callback on exception)
 *   - CheckpointRecord at offset 35 (updated by loop() via checkpoint())
 *
 * The crash record additionally carries the last checkpoint value, so an
 * exception dump is self-contained. The standalone CheckpointRecord covers
 * the hardware-watchdog case where the exception callback never fires.
 */

#include "Crash.h"
#include "Log.h"
#include <Arduino.h>
#include <user_interface.h>   // struct rst_info, REASON_* constants

namespace {

constexpr uint32_t CRASH_OFFSET      = 0;        // dword offset 0
constexpr uint32_t CHECKPOINT_OFFSET = 35;       // dword offset 35 (past CrashRecord)
constexpr uint32_t CRASH_MAGIC       = 0xC0FFEE42u;
constexpr uint32_t CP_MAGIC          = 0xC0DECA11u;
constexpr size_t   STACK_WORDS       = 24;

struct CrashRecord {
  uint32_t magic;
  uint32_t reason;
  uint32_t exccause;
  uint32_t epc1;
  uint32_t epc2;
  uint32_t epc3;
  uint32_t excvaddr;
  uint32_t depc;
  uint32_t stackStart;
  uint32_t stackEnd;
  uint32_t lastCheckpoint;
  uint32_t stack[STACK_WORDS];
};

struct CheckpointRecord {
  uint32_t magic;
  uint32_t module;
};

static_assert(sizeof(CrashRecord) % 4 == 0, "RTC writes must be 4-byte aligned");
static_assert(sizeof(CrashRecord)      == 35 * 4, "CrashRecord size pins CHECKPOINT_OFFSET");
static_assert(sizeof(CheckpointRecord) % 4 == 0, "RTC writes must be 4-byte aligned");
static_assert(sizeof(CrashRecord) + sizeof(CheckpointRecord) <= 512,
              "RTC user memory is 512 bytes total");

// Current subsystem. `volatile` because custom_crash_callback (exception
// context) reads it. Initialised to a value outside the enum so the first
// checkpoint() call always writes.
volatile uint8_t s_lastModule = 0xFF;

const char* const MODULE_NAMES[] = {
  "INIT",        // CP_INIT
  "WEB",         // CP_WEB
  "BLE",         // CP_BLE
  "MDNS",        // CP_MDNS
  "MQTT",        // CP_MQTT
  "MQTT_PEND",   // CP_MQTT_PEND
  "HOOK",        // CP_HOOK
  "TEMP_REQ",    // CP_TEMP_REQ
  "TEMP_READ",   // CP_TEMP_READ
  "PROBE_SCAN",  // CP_PROBE_SCAN
  "TILT",        // CP_TILT
  "FERM",        // CP_FERM
  "CLOUD",       // CP_CLOUD
  "MQTT_PUB",    // CP_MQTT_PUB
  "TEN_MIN",     // CP_TEN_MIN
};
constexpr size_t MODULE_COUNT = sizeof(MODULE_NAMES) / sizeof(MODULE_NAMES[0]);

const char* moduleName(uint32_t id) {
  return (id < MODULE_COUNT) ? MODULE_NAMES[id] : "?";
}

} // namespace

void checkpoint(uint8_t module) {
  if (module == s_lastModule) return;
  s_lastModule = module;
  CheckpointRecord rec = { CP_MAGIC, module };
  ESP.rtcUserMemoryWrite(CHECKPOINT_OFFSET, (uint32_t*)&rec, sizeof(rec));
}

void crashLogPendingDeferred() {
  // Quiet only on hardware power-on / external reset. Soft restarts (code 4)
  // are included because the SDK uses that path for panic()/assert() failures
  // — they look like clean restarts but aren't. For intentional reboots
  // (OTA, /reboot, RESET_CONFIG), the resulting `last=<subsystem>` line is
  // mild noise but tells us which path initiated the restart, so it's worth
  // keeping uniform.
  struct rst_info* ri = ESP.getResetInfoPtr();
  const bool unexpected = (ri->reason == REASON_WDT_RST) ||
                          (ri->reason == REASON_EXCEPTION_RST) ||
                          (ri->reason == REASON_SOFT_WDT_RST) ||
                          (ri->reason == REASON_SOFT_RESTART);
  if (!unexpected) return;

  CrashRecord crashRec;
  const bool haveCrash =
    ESP.rtcUserMemoryRead(CRASH_OFFSET, (uint32_t*)&crashRec, sizeof(crashRec))
    && crashRec.magic == CRASH_MAGIC;

  if (haveCrash) {
    // The existing DEFERRED block already logs exccause/EPC1/EXCVADDR from
    // ESP.getResetInfoPtr(); these lines add the rest of the register frame
    // plus stack + last-known subsystem.
    logMsgL(SYSLOG_ERR,
            "DEFERRED [SYS] Crash detail: EPC2=0x%08x EPC3=0x%08x DEPC=0x%08x last=%s",
            crashRec.epc2, crashRec.epc3, crashRec.depc,
            moduleName(crashRec.lastCheckpoint));
    logMsgL(SYSLOG_ERR,
            "DEFERRED [SYS] Stack: SP=0x%08x end=0x%08x (%u words follow)",
            crashRec.stackStart, crashRec.stackEnd, (unsigned)STACK_WORDS);
    for (size_t i = 0; i < STACK_WORDS; i += 8) {
      logMsgL(SYSLOG_ERR,
              "DEFERRED [SYS] STACK %02u: %08x %08x %08x %08x %08x %08x %08x %08x",
              (unsigned)i,
              crashRec.stack[i+0], crashRec.stack[i+1], crashRec.stack[i+2], crashRec.stack[i+3],
              crashRec.stack[i+4], crashRec.stack[i+5], crashRec.stack[i+6], crashRec.stack[i+7]);
    }
    uint32_t zero = 0;
    ESP.rtcUserMemoryWrite(CRASH_OFFSET, &zero, sizeof(zero));
    return;
  }

  // Non-exception path (hw watchdog, soft watchdog, soft restart). The
  // exception handler never ran, so there's no register frame or stack —
  // the checkpoint breadcrumb is all we have. Label the line with the
  // human-readable reset reason so panic-style soft restarts are visually
  // distinct from intentional reboots in the log.
  CheckpointRecord cpRec;
  const bool haveCp =
    ESP.rtcUserMemoryRead(CHECKPOINT_OFFSET, (uint32_t*)&cpRec, sizeof(cpRec))
    && cpRec.magic == CP_MAGIC;
  if (haveCp) {
    logMsgL(SYSLOG_ERR,
            "DEFERRED [SYS] %s: last subsystem = %s (%u)",
            ESP.getResetReason().c_str(), moduleName(cpRec.module), cpRec.module);
  } else {
    logMsgL(SYSLOG_ERR,
            "DEFERRED [SYS] %s: no checkpoint recorded",
            ESP.getResetReason().c_str());
  }
}

// ============================================================
// custom_crash_callback — called from the exception handler
// before reset. Heap is not safe; stick to register-level work.
// ============================================================
extern "C" void custom_crash_callback(struct rst_info* info,
                                      uint32_t stack,
                                      uint32_t stack_end) {
  CrashRecord rec = {};
  rec.magic          = CRASH_MAGIC;
  rec.reason         = info->reason;
  rec.exccause       = info->exccause;
  rec.epc1           = info->epc1;
  rec.epc2           = info->epc2;
  rec.epc3           = info->epc3;
  rec.excvaddr       = info->excvaddr;
  rec.depc           = info->depc;
  rec.stackStart     = stack;
  rec.stackEnd       = stack_end;
  rec.lastCheckpoint = s_lastModule;

  // Guard against misaligned SP — Xtensa ABI normally guarantees 16-byte alignment
  // but the value passed here at crash time can be unaligned, which would itself
  // trigger an Exception 28 (LoadStoreAlignmentCause) inside the crash handler.
  if (!(stack & 3) && stack < stack_end) {
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(stack);
    size_t available = (stack_end - stack) / sizeof(uint32_t);
    size_t n = available < STACK_WORDS ? available : STACK_WORDS;
    for (size_t i = 0; i < n; i++) rec.stack[i] = sp[i];
  }

  ESP.rtcUserMemoryWrite(CRASH_OFFSET, (uint32_t*)&rec, sizeof(rec));
}
