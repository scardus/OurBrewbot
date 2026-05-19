/*
 * Https.cpp — HTTPS client with TLS state in IRAM second heap
 */

#include "Https.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <umm_malloc/umm_heap_select.h>

int httpsRequest(const char* method,
                 const char* url,
                 const char* contentType,
                 const char* body,
                 const char* authHeader,
                 uint32_t timeoutMs,
                 String* responseOut) {
  if (!method || !url) return -1;
  if (WiFi.status() != WL_CONNECTED) return -2;

  // iramResponse holds the raw response body. Its String header lives on the
  // stack (DRAM); its char buffer is allocated inside the HeapSelectIram scope
  // so it lands in the IRAM heap. After the scope ends we copy it into the
  // caller-supplied DRAM String. iramResponse's destructor at function return
  // frees the IRAM buffer (umm_malloc routes free() by pointer).
  String iramResponse;
  int code = -1;

  {
    HeapSelectIram ephemeral;

    BearSSL::WiFiClientSecure client;
    client.setInsecure();
    // 512/512 byte TLS record buffers — small footprint suitable for IRAM.
    // Requires server MFLN support OR small TLS records; webhook providers
    // (Discord/Pushover/ntfy/Telegram) all support MFLN.
    client.setBufferSizes(512, 512);

    HTTPClient http;
    http.setTimeout(timeoutMs);

    if (!http.begin(client, url)) {
      return -3;
    }

    if (contentType && *contentType) {
      http.addHeader("Content-Type", contentType);
    }

    // Parse "Name: value" auth header into name + value parts.
    if (authHeader && *authHeader) {
      const char* colon = strchr(authHeader, ':');
      if (colon && colon != authHeader) {
        char headerName[40];
        size_t nameLen = colon - authHeader;
        if (nameLen < sizeof(headerName)) {
          memcpy(headerName, authHeader, nameLen);
          headerName[nameLen] = '\0';
          const char* value = colon + 1;
          while (*value == ' ' || *value == '\t') value++;
          http.addHeader(headerName, value);
        }
      }
    }

    size_t bodyLen = body ? strlen(body) : 0;
    code = http.sendRequest(method,
                            (const uint8_t*)(body ? body : ""),
                            bodyLen);

    if (code > 0 && responseOut) {
      iramResponse = http.getString();
    }

    http.end();
  }

  if (responseOut) {
    *responseOut = iramResponse;
  }

  return code;
}
