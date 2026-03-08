#include <gtest/gtest.h>
#include <iostream>

// 这是一个正常的测试用例
TEST(SmokeTest, BasicAssertion) {
    EXPECT_EQ(1 + 1, 2);
}

// 这是一个故意埋下内存泄漏的用例 (稍后我们要用 ASan 抓住它)
TEST(SmokeTest, IntentionalMemoryLeak) {
    int* leaked_array = new int[10];
    leaked_array[0] = 42;
    EXPECT_EQ(leaked_array[0], 42);
    // 故意不 delete[] leaked_array;
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
