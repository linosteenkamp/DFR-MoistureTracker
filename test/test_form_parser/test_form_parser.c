#include <unity.h>
#include <string.h>
#include "../../src/form_parser.c"

void setUp(void) {}
void tearDown(void) {}

static void test_extracts_three_fields(void) {
    char ssid[32] = {0}, pw[32] = {0}, dev[32] = {0};
    form_field_t fields[] = {
        {"ssid",      ssid, sizeof(ssid)},
        {"password",  pw,   sizeof(pw)},
        {"device_id", dev,  sizeof(dev)},
    };
    bool ok = form_parser_extract("ssid=foo&password=bar&device_id=baz", fields, 3);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("foo", ssid);
    TEST_ASSERT_EQUAL_STRING("bar", pw);
    TEST_ASSERT_EQUAL_STRING("baz", dev);
}

static void test_returns_false_when_field_missing(void) {
    char ssid[32] = {0}, pw[32] = {0};
    form_field_t fields[] = {
        {"ssid",     ssid, sizeof(ssid)},
        {"password", pw,   sizeof(pw)},
    };
    bool ok = form_parser_extract("ssid=foo", fields, 2);
    TEST_ASSERT_FALSE(ok);
}

static void test_returns_false_when_value_overflows_buffer(void) {
    char ssid[4] = {0};
    form_field_t fields[] = {{"ssid", ssid, sizeof(ssid)}};
    bool ok = form_parser_extract("ssid=toolong", fields, 1);
    TEST_ASSERT_FALSE(ok);
}

static void test_decodes_plus_as_space(void) {
    char ssid[32] = {0};
    form_field_t fields[] = {{"ssid", ssid, sizeof(ssid)}};
    bool ok = form_parser_extract("ssid=my+net", fields, 1);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("my net", ssid);
}

static void test_handles_trailing_field(void) {
    char a[8] = {0}, b[8] = {0};
    form_field_t fields[] = {{"a", a, sizeof(a)}, {"b", b, sizeof(b)}};
    bool ok = form_parser_extract("a=x&b=y", fields, 2);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("x", a);
    TEST_ASSERT_EQUAL_STRING("y", b);
}

static void test_does_not_match_substring_of_other_field(void) {
    char ssid[16] = {0};
    form_field_t fields[] = {{"ssid", ssid, sizeof(ssid)}};
    // The "xssid=foo&" segment must NOT be picked up for field "ssid";
    // the real value "bar" must win.
    bool ok = form_parser_extract("xssid=foo&ssid=bar", fields, 1);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("bar", ssid);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_extracts_three_fields);
    RUN_TEST(test_returns_false_when_field_missing);
    RUN_TEST(test_returns_false_when_value_overflows_buffer);
    RUN_TEST(test_decodes_plus_as_space);
    RUN_TEST(test_handles_trailing_field);
    RUN_TEST(test_does_not_match_substring_of_other_field);
    return UNITY_END();
}
