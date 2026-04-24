#include <gtest/gtest.h>
#include "minicyber/common/types.h"

using namespace minicyber;

// Test NullType is empty
TEST(TypesTest, NullType) {
    EXPECT_EQ(sizeof(NullType), 1);  // Empty class has size 1 in C++
}

// Test ReturnCode enum values
TEST(TypesTest, ReturnCode) {
    EXPECT_EQ(SUCC, 0);
    EXPECT_EQ(FAIL, 1);

    ReturnCode code = SUCC;
    EXPECT_EQ(code, 0);

    code = FAIL;
    EXPECT_EQ(code, 1);
}

// Test Relation enum values
TEST(TypesTest, Relation) {
    EXPECT_EQ(NO_RELATION, 0);
    EXPECT_EQ(DIFF_HOST, 1);
    EXPECT_EQ(DIFF_PROC, 2);
    EXPECT_EQ(SAME_PROC, 3);

    // Test that Relation is uint8_t sized
    EXPECT_EQ(sizeof(Relation), sizeof(std::uint8_t));
}

// Test Relation in conditional logic
TEST(TypesTest, RelationUsage) {
    Relation rel = SAME_PROC;
    bool is_same_proc = (rel == SAME_PROC);
    EXPECT_TRUE(is_same_proc);

    rel = DIFF_PROC;
    bool is_diff_proc = (rel == DIFF_PROC);
    EXPECT_TRUE(is_diff_proc);

    rel = DIFF_HOST;
    bool is_diff_host = (rel == DIFF_HOST);
    EXPECT_TRUE(is_diff_host);

    rel = NO_RELATION;
    bool no_relation = (rel == NO_RELATION);
    EXPECT_TRUE(no_relation);
}
