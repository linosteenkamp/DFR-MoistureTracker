#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_unity_works(void) {
    TEST_ASSERT_EQUAL_INT(4, 2 + 2);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_unity_works);
    return UNITY_END();
}
