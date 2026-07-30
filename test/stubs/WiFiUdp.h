#pragma once
// Recording stand-in for WiFiUDP. Log.cpp's syslog output is fire-and-forget
// UDP with no return value to inspect, so the packet it would have put on the
// wire is the only observable behaviour - this captures it instead of sending.
//
// Only the three calls Log.cpp makes are implemented (beginPacket / write /
// endPacket); a packet is recorded when endPacket() completes, so a test can
// tell "nothing was sent" from "something was sent".

#include <cstdint>
#include <cstring>
#include <IPAddress.h>

#define UDP_TEST_MAX_PACKETS 8
#define UDP_TEST_MAX_LEN     512

struct UdpTestPacket {
  IPAddress dest;
  uint16_t  port;
  char      data[UDP_TEST_MAX_LEN];
  size_t    len;
};

static UdpTestPacket g_udpPackets[UDP_TEST_MAX_PACKETS];
static int           g_udpPacketCount = 0;

class WiFiUDP {
  UdpTestPacket pending_{};
  bool          open_ = false;
public:
  int beginPacket(IPAddress ip, uint16_t port) {
    pending_      = UdpTestPacket{};
    pending_.dest = ip;
    pending_.port = port;
    open_         = true;
    return 1;
  }

  size_t write(const uint8_t* buf, size_t size) {
    if (!open_) return 0;
    size_t room = UDP_TEST_MAX_LEN - pending_.len;
    if (size > room) size = room;
    memcpy(pending_.data + pending_.len, buf, size);
    pending_.len += size;
    return size;
  }

  int endPacket() {
    if (!open_) return 0;
    open_ = false;
    if (g_udpPacketCount < UDP_TEST_MAX_PACKETS) {
      g_udpPackets[g_udpPacketCount++] = pending_;
    }
    return 1;
  }
};

// ---- test helpers ----

static void udpTestReset() {
  g_udpPacketCount = 0;
  for (int i = 0; i < UDP_TEST_MAX_PACKETS; i++) g_udpPackets[i] = UdpTestPacket{};
}

// The most recent packet's payload as a NUL-terminated string, or "" if none.
static const char* udpTestLastPayload() {
  static char buf[UDP_TEST_MAX_LEN + 1];
  buf[0] = '\0';
  if (g_udpPacketCount == 0) return buf;
  const UdpTestPacket& p = g_udpPackets[g_udpPacketCount - 1];
  memcpy(buf, p.data, p.len);
  buf[p.len] = '\0';
  return buf;
}
