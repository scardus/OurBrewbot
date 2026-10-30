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

// Native (host) tests for OurBrewbot/FirmwareCheck.cpp: a firmware upload is
// only installed if it is complete and undamaged.
//
// FirmwareCheck.cpp is #included directly (not linked) so the real,
// unmodified production source is what's under test.

#include <unity.h>
#include <cstdint>

#include "../../OurBrewbot/FirmwareCheck.cpp"
#include "firmware_image.h"

static FirmwareCheck check;

void setUp(void) {
  makeTestImage();
  firmwareCheckStart(check);
}

void tearDown(void) {}

// The crc32() stub must be the core's CRC: this is that CRC's standard check value
void test_crc32_gives_the_standard_check_value(void) {
  TEST_ASSERT_EQUAL_HEX32(0x0376E6E7, crc32("123456789", 9));
}

void test_a_complete_image_passes(void) {
  firmwareCheckAdd(check, g_testImage, TEST_IMAGE_SIZE);
  TEST_ASSERT_TRUE(firmwareCheckPassed(check));
}

void test_an_image_in_uneven_chunks_passes(void) {
  // Chunks that start and end inside the length and CRC
  size_t sizes[] = { 1, 1460, 2651, 3, 2, 5, 1, 877 };
  size_t at = 0;
  for (size_t s : sizes) {
    firmwareCheckAdd(check, g_testImage + at, s);
    at += s;
  }
  TEST_ASSERT_EQUAL_UINT32(TEST_IMAGE_SIZE, at);
  TEST_ASSERT_TRUE(firmwareCheckPassed(check));
}

void test_an_image_one_byte_at_a_time_passes(void) {
  for (size_t i = 0; i < TEST_IMAGE_SIZE; i++) firmwareCheckAdd(check, g_testImage + i, 1);
  TEST_ASSERT_TRUE(firmwareCheckPassed(check));
}

void test_a_cut_short_image_fails(void) {
  firmwareCheckAdd(check, g_testImage, TEST_IMAGE_SIZE - 1);
  TEST_ASSERT_FALSE(firmwareCheckPassed(check));
}

void test_an_image_with_extra_bytes_fails(void) {
  static const uint8_t extra = 0;
  firmwareCheckAdd(check, g_testImage, TEST_IMAGE_SIZE);
  firmwareCheckAdd(check, &extra, 1);
  TEST_ASSERT_FALSE(firmwareCheckPassed(check));
}

void test_a_damaged_byte_fails(void) {
  g_testImage[3000] ^= 0x01;
  firmwareCheckAdd(check, g_testImage, TEST_IMAGE_SIZE);
  TEST_ASSERT_FALSE(firmwareCheckPassed(check));
}

void test_a_wrong_crc_fails(void) {
  storeWord(g_testImage + 4116, TEST_IMAGE_CRC + 1);
  firmwareCheckAdd(check, g_testImage, TEST_IMAGE_SIZE);
  TEST_ASSERT_FALSE(firmwareCheckPassed(check));
}

void test_a_file_too_short_to_hold_the_crc_fails(void) {
  firmwareCheckAdd(check, g_testImage, 4119);
  TEST_ASSERT_FALSE(firmwareCheckPassed(check));
}

void test_an_empty_upload_fails(void) {
  TEST_ASSERT_FALSE(firmwareCheckPassed(check));
}

void test_starting_again_forgets_the_last_upload(void) {
  firmwareCheckAdd(check, g_testImage, 100);
  firmwareCheckStart(check);
  firmwareCheckAdd(check, g_testImage, TEST_IMAGE_SIZE);
  TEST_ASSERT_TRUE(firmwareCheckPassed(check));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_crc32_gives_the_standard_check_value);
  RUN_TEST(test_a_complete_image_passes);
  RUN_TEST(test_an_image_in_uneven_chunks_passes);
  RUN_TEST(test_an_image_one_byte_at_a_time_passes);
  RUN_TEST(test_a_cut_short_image_fails);
  RUN_TEST(test_an_image_with_extra_bytes_fails);
  RUN_TEST(test_a_damaged_byte_fails);
  RUN_TEST(test_a_wrong_crc_fails);
  RUN_TEST(test_a_file_too_short_to_hold_the_crc_fails);
  RUN_TEST(test_an_empty_upload_fails);
  RUN_TEST(test_starting_again_forgets_the_last_upload);
  return UNITY_END();
}
