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

// Packet `index`'s payload as a NUL-terminated string, or "" if there is no
// such packet. The returned buffer is reused by the next call.
static const char* udpTestPayload(int index) {
  static char buf[UDP_TEST_MAX_LEN + 1];
  buf[0] = '\0';
  if (index < 0 || index >= g_udpPacketCount) return buf;
  const UdpTestPacket& p = g_udpPackets[index];
  memcpy(buf, p.data, p.len);
  buf[p.len] = '\0';
  return buf;
}

// The most recent packet's payload as a NUL-terminated string, or "" if none.
static const char* udpTestLastPayload() {
  return udpTestPayload(g_udpPacketCount - 1);
}
