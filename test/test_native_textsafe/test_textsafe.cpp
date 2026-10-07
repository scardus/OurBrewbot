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

// ============================================================
// copyText()
// ============================================================

// A field with guard bytes after it, so a test can check nothing was written
// past the size it was given.
struct GuardedField {
  char text[8];
  char guard[4];
};

static void fillGuarded(GuardedField& f) {
  memset(&f, 'X', sizeof(f));
}

static void assertGuardIntact(const GuardedField& f) {
  for (size_t k = 0; k < sizeof(f.guard); k++) TEST_ASSERT_EQUAL_CHAR('X', f.guard[k]);
}

void test_copy_plain_text_is_unchanged(void) {
  GuardedField f;
  fillGuarded(f);
  copyText(f.text, "Pale", sizeof(f.text));
  TEST_ASSERT_EQUAL_STRING("Pale", f.text);
  assertGuardIntact(f);
}

void test_copy_text_that_exactly_fills_the_field(void) {
  GuardedField f;
  fillGuarded(f);
  copyText(f.text, "1234567", sizeof(f.text));   // 7 chars + NUL = 8
  TEST_ASSERT_EQUAL_STRING("1234567", f.text);
  assertGuardIntact(f);
}

void test_copy_cuts_long_plain_text_like_strlcpy(void) {
  GuardedField f;
  fillGuarded(f);
  copyText(f.text, "Imperial Stout", sizeof(f.text));
  TEST_ASSERT_EQUAL_STRING("Imperia", f.text);
  assertGuardIntact(f);
}

void test_copy_keeps_accented_characters(void) {
  GuardedField f;
  fillGuarded(f);
  copyText(f.text, "Bi\xC3\xA8re", sizeof(f.text));   // "Biere" with e grave
  TEST_ASSERT_EQUAL_STRING("Bi\xC3\xA8re", f.text);
  assertGuardIntact(f);
}

void test_copy_never_cuts_a_character_in_half(void) {
  // "Weisse" with a sharp s at bytes 6-7 of an 8-byte field: only 7 bytes
  // fit, so strlcpy() would keep the first half of it. The whole character
  // is dropped instead.
  GuardedField f;
  fillGuarded(f);
  copyText(f.text, "Weisse\xC3\x9F", sizeof(f.text));
  TEST_ASSERT_EQUAL_STRING("Weisse", f.text);
  assertGuardIntact(f);
}

void test_copy_drops_a_four_byte_character_that_does_not_fit(void) {
  GuardedField f;
  fillGuarded(f);
  copyText(f.text, "Beer\xF0\x9F\x8D\xBA", sizeof(f.text));   // 4 + 4 bytes
  TEST_ASSERT_EQUAL_STRING("Beer", f.text);
  assertGuardIntact(f);
}

void test_copy_replaces_invalid_bytes_with_a_question_mark(void) {
  GuardedField f;
  fillGuarded(f);
  copyText(f.text, "a\xFF" "b\xC3" "c", sizeof(f.text));
  TEST_ASSERT_EQUAL_STRING("a?b?c", f.text);
  assertGuardIntact(f);
}

void test_copy_replaces_a_character_already_cut_in_half(void) {
  // What older firmware stored when it cut a name with strlcpy().
  GuardedField f;
  fillGuarded(f);
  copyText(f.text, "Weisse\xC3", sizeof(f.text));
  TEST_ASSERT_EQUAL_STRING("Weisse?", f.text);
  assertGuardIntact(f);
}

void test_copy_of_empty_text(void) {
  GuardedField f;
  fillGuarded(f);
  copyText(f.text, "", sizeof(f.text));
  TEST_ASSERT_EQUAL_STRING("", f.text);
  assertGuardIntact(f);
}

void test_copy_into_a_one_byte_field_is_just_the_nul(void) {
  char one[1] = { 'X' };
  copyText(one, "abc", sizeof(one));
  TEST_ASSERT_EQUAL_CHAR('\0', one[0]);
}

// ============================================================
// isValidBaseTopic()
// ============================================================

// The base topic field is char[32] in MqttConfig.
static const size_t kTopicField = 32;

void test_base_topic_plain_name_is_valid(void) {
  TEST_ASSERT_TRUE(isValidBaseTopic("ourbrewbot", kTopicField));
}

void test_base_topic_may_have_levels_spaces_and_accents(void) {
  TEST_ASSERT_TRUE(isValidBaseTopic("home/brewery", kTopicField));
  TEST_ASSERT_TRUE(isValidBaseTopic("brew bot", kTopicField));
  TEST_ASSERT_TRUE(isValidBaseTopic("bi\xC3\xA8re", kTopicField));
}

void test_base_topic_must_not_be_empty(void) {
  TEST_ASSERT_FALSE(isValidBaseTopic("", kTopicField));
}

void test_base_topic_must_fit_the_field_whole(void) {
  TEST_ASSERT_TRUE (isValidBaseTopic("abcdefghijklmnopqrstuvwxyz12345", kTopicField));   // 31
  TEST_ASSERT_FALSE(isValidBaseTopic("abcdefghijklmnopqrstuvwxyz123456", kTopicField));  // 32
}

void test_base_topic_must_not_hold_a_wildcard(void) {
  TEST_ASSERT_FALSE(isValidBaseTopic("brew/+", kTopicField));
  TEST_ASSERT_FALSE(isValidBaseTopic("brew/#", kTopicField));
  TEST_ASSERT_FALSE(isValidBaseTopic("a+b", kTopicField));
}

void test_base_topic_must_not_start_with_a_dollar(void) {
  TEST_ASSERT_FALSE(isValidBaseTopic("$SYS", kTopicField));
  TEST_ASSERT_TRUE (isValidBaseTopic("brew$", kTopicField));   // only the first character
}

void test_base_topic_must_not_hold_control_characters(void) {
  TEST_ASSERT_FALSE(isValidBaseTopic("brew\tbot", kTopicField));
  TEST_ASSERT_FALSE(isValidBaseTopic("brew\nbot", kTopicField));
  TEST_ASSERT_FALSE(isValidBaseTopic("brew\x7F", kTopicField));
}

void test_base_topic_must_be_valid_utf8(void) {
  TEST_ASSERT_FALSE(isValidBaseTopic("brew\xFF", kTopicField));
  TEST_ASSERT_FALSE(isValidBaseTopic("brew\xC3", kTopicField));
}

// ============================================================
// makeBaseTopicSafe()
// ============================================================

void test_make_safe_leaves_a_working_topic_alone(void) {
  char topic[32] = "home/brew bot/bi\xC3\xA8re";
  TEST_ASSERT_FALSE(makeBaseTopicSafe(topic));
  TEST_ASSERT_EQUAL_STRING("home/brew bot/bi\xC3\xA8re", topic);
}

void test_make_safe_replaces_wildcards(void) {
  char topic[32] = "brew/+/#";
  TEST_ASSERT_TRUE(makeBaseTopicSafe(topic));
  TEST_ASSERT_EQUAL_STRING("brew/_/_", topic);
}

void test_make_safe_replaces_only_a_leading_dollar(void) {
  char topic[32] = "$brew$";
  TEST_ASSERT_TRUE(makeBaseTopicSafe(topic));
  TEST_ASSERT_EQUAL_STRING("_brew$", topic);
}

void test_make_safe_replaces_control_characters(void) {
  char topic[32] = "brew\tbot\x7F";
  TEST_ASSERT_TRUE(makeBaseTopicSafe(topic));
  TEST_ASSERT_EQUAL_STRING("brew_bot_", topic);
}

void test_make_safe_result_passes_the_save_check(void) {
  // Whatever a stored topic held, once repaired it is one POST /mqtt would
  // accept (the loader has already made it valid UTF-8 and non-empty).
  char topic[32] = "$a+b#c\x01" "d";
  makeBaseTopicSafe(topic);
  TEST_ASSERT_TRUE(isValidBaseTopic(topic, sizeof(topic)));
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

  RUN_TEST(test_copy_plain_text_is_unchanged);
  RUN_TEST(test_copy_text_that_exactly_fills_the_field);
  RUN_TEST(test_copy_cuts_long_plain_text_like_strlcpy);
  RUN_TEST(test_copy_keeps_accented_characters);
  RUN_TEST(test_copy_never_cuts_a_character_in_half);
  RUN_TEST(test_copy_drops_a_four_byte_character_that_does_not_fit);
  RUN_TEST(test_copy_replaces_invalid_bytes_with_a_question_mark);
  RUN_TEST(test_copy_replaces_a_character_already_cut_in_half);
  RUN_TEST(test_copy_of_empty_text);
  RUN_TEST(test_copy_into_a_one_byte_field_is_just_the_nul);

  RUN_TEST(test_base_topic_plain_name_is_valid);
  RUN_TEST(test_base_topic_may_have_levels_spaces_and_accents);
  RUN_TEST(test_base_topic_must_not_be_empty);
  RUN_TEST(test_base_topic_must_fit_the_field_whole);
  RUN_TEST(test_base_topic_must_not_hold_a_wildcard);
  RUN_TEST(test_base_topic_must_not_start_with_a_dollar);
  RUN_TEST(test_base_topic_must_not_hold_control_characters);
  RUN_TEST(test_base_topic_must_be_valid_utf8);

  RUN_TEST(test_make_safe_leaves_a_working_topic_alone);
  RUN_TEST(test_make_safe_replaces_wildcards);
  RUN_TEST(test_make_safe_replaces_only_a_leading_dollar);
  RUN_TEST(test_make_safe_replaces_control_characters);
  RUN_TEST(test_make_safe_result_passes_the_save_check);

  return UNITY_END();
}
