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

// Native (host) tests for OurBrewbot/TextSafe.cpp - the checks applied to
// text that comes from outside the firmware (the WebUI, devices, imported
// config files). TextSafe.cpp has no Arduino dependencies, so it is included
// directly with no stubs.

#include <unity.h>
#include <cstring>

#include "../../OurBrewbot/TextSafe.cpp"

void setUp(void) {}
void tearDown(void) {}

// utf8CharLength() takes unsigned bytes; string literals are char.
static size_t charLen(const char* s) {
  return utf8CharLength((const unsigned char*)s);
}

// ============================================================
// utf8CharLength()
// ============================================================

void test_utf8_two_byte_character(void) {
  TEST_ASSERT_EQUAL(2, charLen("\xC3\xA9"));          // e acute
}

void test_utf8_three_byte_character(void) {
  TEST_ASSERT_EQUAL(3, charLen("\xE2\x82\xAC"));      // euro sign
}

void test_utf8_four_byte_character(void) {
  TEST_ASSERT_EQUAL(4, charLen("\xF0\x9F\x8D\xBA"));  // beer mug emoji
}

void test_utf8_ascii_is_not_a_multibyte_character(void) {
  // ASCII is handled by the callers before they ask; 0 means "not a valid
  // 2-4 byte character here".
  TEST_ASSERT_EQUAL(0, charLen("A"));
}

void test_utf8_lone_continuation_byte_is_invalid(void) {
  TEST_ASSERT_EQUAL(0, charLen("\xA9"));
}

void test_utf8_character_cut_short_by_the_end_of_the_string_is_invalid(void) {
  // A name cut at its size limit half way through a character: the NUL is
  // not a continuation byte, so the check stops there without reading past it.
  TEST_ASSERT_EQUAL(0, charLen("\xC3"));
  TEST_ASSERT_EQUAL(0, charLen("\xE2\x82"));
  TEST_ASSERT_EQUAL(0, charLen("\xF0\x9F\x8D"));
}

void test_utf8_character_followed_by_ascii_is_invalid(void) {
  TEST_ASSERT_EQUAL(0, charLen("\xC3" "A"));
}

void test_utf8_overlong_encodings_are_invalid(void) {
  TEST_ASSERT_EQUAL(0, charLen("\xC0\xAF"));          // '/' in 2 bytes
  TEST_ASSERT_EQUAL(0, charLen("\xC1\xBF"));
  TEST_ASSERT_EQUAL(0, charLen("\xE0\x80\xAF"));      // '/' in 3 bytes
  TEST_ASSERT_EQUAL(0, charLen("\xF0\x80\x80\xAF"));  // '/' in 4 bytes
}

void test_utf8_surrogates_are_invalid(void) {
  TEST_ASSERT_EQUAL(0, charLen("\xED\xA0\x80"));      // U+D800
  TEST_ASSERT_EQUAL(3, charLen("\xED\x9F\xBF"));      // U+D7FF, just below
}

void test_utf8_above_the_unicode_range_is_invalid(void) {
  TEST_ASSERT_EQUAL(4, charLen("\xF4\x8F\xBF\xBF"));  // U+10FFFF, the last
  TEST_ASSERT_EQUAL(0, charLen("\xF4\x90\x80\x80"));  // U+110000
  TEST_ASSERT_EQUAL(0, charLen("\xF5\x80\x80\x80"));
  TEST_ASSERT_EQUAL(0, charLen("\xFF"));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_utf8_two_byte_character);
  RUN_TEST(test_utf8_three_byte_character);
  RUN_TEST(test_utf8_four_byte_character);
  RUN_TEST(test_utf8_ascii_is_not_a_multibyte_character);
  RUN_TEST(test_utf8_lone_continuation_byte_is_invalid);
  RUN_TEST(test_utf8_character_cut_short_by_the_end_of_the_string_is_invalid);
  RUN_TEST(test_utf8_character_followed_by_ascii_is_invalid);
  RUN_TEST(test_utf8_overlong_encodings_are_invalid);
  RUN_TEST(test_utf8_surrogates_are_invalid);
  RUN_TEST(test_utf8_above_the_unicode_range_is_invalid);

  return UNITY_END();
}
