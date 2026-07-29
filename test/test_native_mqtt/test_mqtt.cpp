// Native (host) tests for the MQTT command-topic parsing functions extracted
// from mqttMessageCallback() in OurBrewbot/Mqtt.cpp: parseMqttCommandTopic()
// and fermenterIndexFromScope().
//
// Unlike the other test_native_* suites, this one does NOT #include the whole
// production .cpp - Mqtt.cpp pulls in ESP8266WiFi.h/PubSubClient.h and
// instantiates a WiFiClient/PubSubClient at file scope, none of which are
// stubbed (or worth stubbing just for this). The two functions under test
// were extracted into their own MqttParse.h/.cpp specifically because they
// have no such dependency - only Config.h, for MAX_FERMENTERS.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../OurBrewbot/MqttParse.cpp"

void setUp(void) {}
void tearDown(void) {}

// ---- parseMqttCommandTopic ----

void test_parse_valid_fermenter_topic(void) {
  char scope[16], key[32];
  TEST_ASSERT_TRUE(parseMqttCommandTopic("brewbot/Fermenter0/power/set", "brewbot",
                                          scope, sizeof(scope), key, sizeof(key)));
  TEST_ASSERT_EQUAL_STRING("Fermenter0", scope);
  TEST_ASSERT_EQUAL_STRING("power", key);
}

void test_parse_valid_device_topic(void) {
  char scope[16], key[32];
  TEST_ASSERT_TRUE(parseMqttCommandTopic("brewbot/Device/reboot/set", "brewbot",
                                          scope, sizeof(scope), key, sizeof(key)));
  TEST_ASSERT_EQUAL_STRING("Device", scope);
  TEST_ASSERT_EQUAL_STRING("reboot", key);
}

void test_parse_rejects_wrong_base_prefix(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("other/Fermenter0/power/set", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_missing_slash_after_base(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbotX/Fermenter0/power/set", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_missing_scope_segment(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot/Fermenter0", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_empty_scope(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot//power/set", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_scope_too_long(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot/ThisScopeIsWayTooLong/power/set", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_missing_set_suffix(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot/Fermenter0/power", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_wrong_suffix(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot/Fermenter0/power/get", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_empty_key(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic("brewbot/Fermenter0//set", "brewbot",
                                           scope, sizeof(scope), key, sizeof(key)));
}

void test_parse_rejects_key_too_long(void) {
  char scope[16], key[32];
  TEST_ASSERT_FALSE(parseMqttCommandTopic(
      "brewbot/Fermenter0/this_key_name_is_far_too_long_to_fit/set", "brewbot",
      scope, sizeof(scope), key, sizeof(key)));
}

// ---- fermenterIndexFromScope ----

void test_index_first_fermenter(void) {
  TEST_ASSERT_EQUAL_INT(0, fermenterIndexFromScope("Fermenter0"));
}

void test_index_last_valid_fermenter(void) {
  // MAX_FERMENTERS is 4, so valid indices are 0-3.
  TEST_ASSERT_EQUAL_INT(3, fermenterIndexFromScope("Fermenter3"));
}

void test_index_rejects_out_of_range_at_boundary(void) {
  TEST_ASSERT_EQUAL_INT(-1, fermenterIndexFromScope("Fermenter4"));
}

void test_index_rejects_far_out_of_range(void) {
  TEST_ASSERT_EQUAL_INT(-1, fermenterIndexFromScope("Fermenter99"));
}

void test_index_rejects_non_fermenter_scope(void) {
  TEST_ASSERT_EQUAL_INT(-1, fermenterIndexFromScope("Device"));
}

void test_index_rejects_wrong_case(void) {
  TEST_ASSERT_EQUAL_INT(-1, fermenterIndexFromScope("fermenter0"));
}

void test_index_nonnumeric_suffix_is_preexisting_atoi_quirk(void) {
  // atoi() on a non-numeric string returns 0, not an error - this was true of
  // the inline code before extraction too. Documenting current behaviour, not
  // asserting it's correct: "FermenterX" is treated the same as "Fermenter0".
  TEST_ASSERT_EQUAL_INT(0, fermenterIndexFromScope("FermenterX"));
}

void test_index_no_digits_is_preexisting_atoi_quirk(void) {
  // Same quirk: atoi("") also returns 0.
  TEST_ASSERT_EQUAL_INT(0, fermenterIndexFromScope("Fermenter"));
}

int main(int argc, char** argv) {
  UNITY_BEGIN();

  RUN_TEST(test_parse_valid_fermenter_topic);
  RUN_TEST(test_parse_valid_device_topic);
  RUN_TEST(test_parse_rejects_wrong_base_prefix);
  RUN_TEST(test_parse_rejects_missing_slash_after_base);
  RUN_TEST(test_parse_rejects_missing_scope_segment);
  RUN_TEST(test_parse_rejects_empty_scope);
  RUN_TEST(test_parse_rejects_scope_too_long);
  RUN_TEST(test_parse_rejects_missing_set_suffix);
  RUN_TEST(test_parse_rejects_wrong_suffix);
  RUN_TEST(test_parse_rejects_empty_key);
  RUN_TEST(test_parse_rejects_key_too_long);

  RUN_TEST(test_index_first_fermenter);
  RUN_TEST(test_index_last_valid_fermenter);
  RUN_TEST(test_index_rejects_out_of_range_at_boundary);
  RUN_TEST(test_index_rejects_far_out_of_range);
  RUN_TEST(test_index_rejects_non_fermenter_scope);
  RUN_TEST(test_index_rejects_wrong_case);
  RUN_TEST(test_index_nonnumeric_suffix_is_preexisting_atoi_quirk);
  RUN_TEST(test_index_no_digits_is_preexisting_atoi_quirk);

  return UNITY_END();
}
