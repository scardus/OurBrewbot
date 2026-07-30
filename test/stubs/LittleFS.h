#pragma once
// In-memory stand-in for LittleFS, so Config.cpp's persistence layer can be
// tested natively: files live in a fixed array of RAM buffers instead of flash.
//
// The File class implements exactly the four methods ArduinoJson's default
// Reader/Writer need (read/readBytes and write/write) plus close() and the
// bool conversion Config.cpp checks - not a full Arduino FS emulation, extend
// only as new native tests need it.
//
// Two failure modes are settable, because Config.cpp has error paths that are
// otherwise unreachable: fsTestSetFull() makes open-for-write fail outright,
// and fsTestSetWriteLimit() caps the total bytes accepted so a serialize can
// come up short (the "filesystem full mid-write" case saveJsonDocToFile()
// detects by comparing written against measureJson).

#include <cstdint>
#include <cstring>
#include <cstddef>
// For String: WebAPI.cpp opens paths built from request arguments, so the
// String overloads below need the type. Arduino.h does not include this header
// back, so there is no cycle.
#include <Arduino.h>

// 4 KB per file covers the largest config this firmware writes (jsonFermenter
// is 33 fields x 4 slots, jsonProfileSteps is 6 fields x 60 slots - both come
// in under 2.5 KB). 32 slots covers every config file plus its backup (10 x 2
// for a full saveAllConfig), the reboot log and the WiFi artifacts.
#define FS_FILE_CAPACITY 4096
#define FS_MAX_FILES     32

struct FsEntry {
  char   path[48];
  char   data[FS_FILE_CAPACITY];
  size_t len;
  bool   used;
};

static FsEntry g_fsEntries[FS_MAX_FILES];
static bool    g_fsFull        = false;   // open-for-write always fails
static long    g_fsWriteLimit  = -1;      // bytes still accepted; -1 = unlimited

class File {
  FsEntry* e_   = nullptr;
  size_t   pos_ = 0;
public:
  File() {}
  explicit File(FsEntry* e) : e_(e) {}

  explicit operator bool() const { return e_ != nullptr; }

  // ---- reader side (ArduinoJson's default Reader) ----
  int read() {
    if (!e_ || pos_ >= e_->len) return -1;
    return (uint8_t)e_->data[pos_++];
  }
  size_t readBytes(char* buffer, size_t length) {
    if (!e_) return 0;
    size_t avail = e_->len - pos_;
    size_t n     = (length < avail) ? length : avail;
    memcpy(buffer, e_->data + pos_, n);
    pos_ += n;
    return n;
  }

  // ---- writer side (ArduinoJson's default Writer) ----
  // Returns short - rather than failing - when the budget or the buffer runs
  // out, which is how a real partial write presents to serializeJson().
  size_t write(uint8_t c) {
    const uint8_t one = c;
    return write(&one, 1);
  }
  size_t write(const uint8_t* s, size_t n) {
    if (!e_) return 0;
    // Clamp to the buffer first, then to the budget, so the budget is only
    // charged for bytes that were actually stored.
    size_t room = FS_FILE_CAPACITY - e_->len;
    if (n > room) n = room;
    if (g_fsWriteLimit >= 0) {
      size_t budget = (size_t)g_fsWriteLimit;
      if (n > budget) n = budget;
      g_fsWriteLimit -= (long)n;
    }
    memcpy(e_->data + e_->len, s, n);
    e_->len += n;
    return n;
  }

  size_t size() const { return e_ ? e_->len : 0; }
  void   close()      { e_ = nullptr; pos_ = 0; }

  // print() is how WebAPI.cpp's handleFsFileSave() writes a body, as opposed to
  // the serializeJson() path Config.cpp uses.
  size_t print(const char* s) {
    return s ? write((const uint8_t*)s, strlen(s)) : 0;
  }
  size_t print(const String& s) { return print(s.c_str()); }
};

// Directory iteration, for WebAPI.cpp's handleFsFiles(). The real Dir walks
// LittleFS entries; this walks the same in-memory table, skipping free slots.
// next() must be called before the first fileName(), matching the real API.
class Dir {
  int idx_ = -1;
public:
  bool next() {
    for (int i = idx_ + 1; i < FS_MAX_FILES; i++) {
      if (g_fsEntries[i].used) { idx_ = i; return true; }
    }
    idx_ = FS_MAX_FILES;
    return false;
  }
  String fileName() {
    return (idx_ >= 0 && idx_ < FS_MAX_FILES) ? String(g_fsEntries[idx_].path) : String("");
  }
  size_t fileSize() {
    return (idx_ >= 0 && idx_ < FS_MAX_FILES) ? g_fsEntries[idx_].len : 0;
  }
};

class LittleFSStub {
  static FsEntry* find(const char* path) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
      if (g_fsEntries[i].used && strcmp(g_fsEntries[i].path, path) == 0) {
        return &g_fsEntries[i];
      }
    }
    return nullptr;
  }
  static FsEntry* create(const char* path) {
    for (int i = 0; i < FS_MAX_FILES; i++) {
      if (!g_fsEntries[i].used) {
        g_fsEntries[i].used = true;
        strncpy(g_fsEntries[i].path, path, sizeof(g_fsEntries[i].path) - 1);
        g_fsEntries[i].path[sizeof(g_fsEntries[i].path) - 1] = '\0';
        g_fsEntries[i].len = 0;
        return &g_fsEntries[i];
      }
    }
    return nullptr;   // out of slots
  }
public:
  bool begin() { return true; }

  bool exists(const char* path) { return find(path) != nullptr; }

  bool remove(const char* path) {
    FsEntry* e = find(path);
    if (!e) return false;
    e->used = false;
    e->len  = 0;
    return true;
  }

  // WebAPI.cpp passes a String path (built from a request argument), so both
  // overloads are needed; Config.cpp uses the char* one throughout.
  bool exists(const String& path) { return exists(path.c_str()); }

  Dir openDir(const char* /*prefix*/) { return Dir(); }

  File open(const char* path, const char* mode) {
    if (mode && mode[0] == 'r') {
      FsEntry* e = find(path);
      return e ? File(e) : File();
    }
    // write / append
    if (g_fsFull) return File();
    FsEntry* e = find(path);
    if (!e) e = create(path);
    if (!e) return File();
    if (mode && mode[0] == 'w') e->len = 0;   // truncate
    return File(e);
  }
  File open(const String& path, const char* mode) { return open(path.c_str(), mode); }
};

static LittleFSStub LittleFS;

// ---- test helpers ----

static void fsTestReset() {
  for (int i = 0; i < FS_MAX_FILES; i++) {
    g_fsEntries[i].used = false;
    g_fsEntries[i].len  = 0;
  }
  g_fsFull       = false;
  g_fsWriteLimit = -1;
}

// Place a file directly, bypassing the config writers - this is how tests
// stage a legacy or hand-corrupted config for a loader to find.
static void fsTestWrite(const char* path, const char* content) {
  File f = LittleFS.open(path, "w");
  f.write((const uint8_t*)content, strlen(content));
  f.close();
}

// Contents of a file as a NUL-terminated string, or "" when absent. Points at
// a static buffer, so copy it if two files need comparing at once.
static const char* fsTestRead(const char* path) {
  static char buf[FS_FILE_CAPACITY + 1];
  buf[0] = '\0';
  File f = LittleFS.open(path, "r");
  if (!f) return buf;
  size_t n = f.readBytes(buf, FS_FILE_CAPACITY);
  buf[n] = '\0';
  f.close();
  return buf;
}

static void fsTestSetFull(bool full)      { g_fsFull = full; }
static void fsTestSetWriteLimit(long n)   { g_fsWriteLimit = n; }
