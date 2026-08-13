#include <gtest/gtest.h>
#include <iostream>

// 这是一个正常的测试用例
TEST(SmokeTest, BasicAssertion) {
    EXPECT_EQ(1 + 1, 2);
}

TEST(SmokeTest, AllocatedMemoryIsReleased) {
    int* values = new int[10];
    values[0] = 42;
    EXPECT_EQ(values[0], 42);
    delete[] values;
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
