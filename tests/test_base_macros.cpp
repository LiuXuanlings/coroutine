#include <gtest/gtest.h>
#include <stdexcept>
#include "minicyber/base/macros.h"

// Test cyber_likely and cyber_unlikely macros
TEST(MacrosTest, LikelyMacro) {
    int x = 1;
    // These should compile without error
    if (cyber_likely(x == 1)) {
        EXPECT_TRUE(true);
    } else {
        EXPECT_TRUE(false);
    }

    if (cyber_unlikely(x == 0)) {
        EXPECT_TRUE(false);
    } else {
        EXPECT_TRUE(true);
    }
}

// Test CACHELINE_SIZE constant
TEST(MacrosTest, CacheLineSize) {
    EXPECT_EQ(CACHELINE_SIZE, 64);
}

// Test CheckedMalloc with valid size
TEST(MacrosTest, CheckedMallocSuccess) {
    void* ptr = CheckedMalloc(100);
    EXPECT_NE(ptr, nullptr);
    std::free(ptr);
}

// Test CheckedMalloc with zero size (implementation-defined, but should not crash)
TEST(MacrosTest, CheckedMallocZero) {
    // zero size malloc returns either NULL or a unique pointer that can be freed
    void* ptr = CheckedMalloc(0);
    std::free(ptr);
    SUCCEED();
}

// Test CheckedCalloc with valid size
TEST(MacrosTest, CheckedCallocSuccess) {
    void* ptr = CheckedCalloc(10, sizeof(int));
    EXPECT_NE(ptr, nullptr);

    // calloc should zero-initialize memory
    int* int_ptr = static_cast<int*>(ptr);
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(int_ptr[i], 0);
    }

    std::free(ptr);
}

// Test cpu_relax compiles and executes
TEST(MacrosTest, CpuRelax) {
    // Should just compile and run without error
    cpu_relax();
    SUCCEED();
}

// Test DEFINE_TYPE_TRAIT macro
// Define a trait to check if a type has a 'size' method
DEFINE_TYPE_TRAIT(HasSize, size)

class WithSize {
public:
    size_t size() const { return 42; }
};

class WithoutSize {};

TEST(MacrosTest, DefineTypeTrait) {
    EXPECT_TRUE(HasSize<WithSize>::value);
    EXPECT_FALSE(HasSize<WithoutSize>::value);
}
