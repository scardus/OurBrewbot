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

/*
 * TextSafe.cpp — checks for text that comes from outside the firmware
 *
 * See TextSafe.h.
 */

#include "TextSafe.h"
#include <string.h>

size_t utf8CharLength(const unsigned char* s) {
  size_t len;
  unsigned char lo = 0x80, hi = 0xBF;   // allowed range of the second byte
  if (s[0] >= 0xC2 && s[0] <= 0xDF) {
    len = 2;
  } else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
    len = 3;
    if (s[0] == 0xE0) lo = 0xA0;   // no over-long encodings
    if (s[0] == 0xED) hi = 0x9F;   // no UTF-16 surrogates
  } else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
    len = 4;
    if (s[0] == 0xF0) lo = 0x90;   // no over-long encodings
    if (s[0] == 0xF4) hi = 0x8F;   // nothing above U+10FFFF
  } else {
    return 0;
  }
  if (s[1] < lo || s[1] > hi) return 0;
  for (size_t k = 2; k < len; k++) {
    if (s[k] < 0x80 || s[k] > 0xBF) return 0;
  }
  return len;
}

void copyText(char* dst, const char* src, size_t size) {
  if (size == 0) return;
  size_t j = 0;                          // bytes written to dst so far
  size_t i = 0;                          // read position in src
  while (src[i] != '\0') {
    unsigned char c = (unsigned char)src[i];
    size_t inLen  = 1;                   // bytes this character takes in src
    size_t outLen = 1;                   // bytes it takes in dst
    bool   valid  = true;
    if (c >= 0x80) {
      inLen = utf8CharLength((const unsigned char*)src + i);
      if (inLen == 0) {                  // not valid UTF-8: one byte becomes '?'
        inLen = 1;
        valid = false;
      }
      outLen = valid ? inLen : 1;
    }
    if (j + outLen > size - 1) break;    // the whole character won't fit
    if (valid) {
      memcpy(dst + j, src + i, outLen);
    } else {
      dst[j] = '?';
    }
    j += outLen;
    i += inLen;
  }
  dst[j] = '\0';
}
