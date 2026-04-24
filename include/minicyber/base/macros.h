#ifndef MINICYBER_BASE_MACROS_H_
#define MINICYBER_BASE_MACROS_H_

#include <cstdlib>   // 提供 malloc / free / calloc
#include <new>       // 提供 std::bad_alloc 内存分配失败异常

// =============================================================================
// 分支预测宏：给编译器提示分支概率，提升CPU执行效率
// __builtin_expect 是 GCC/Clang 内置指令，用于告诉编译器条件的预期结果
// =============================================================================
#if __GNUC__ >= 3 // GCC 3.0 及以上版本支持 __builtin_expect
// 条件 x 大概率为真
#define cyber_likely(x) (__builtin_expect((x), 1))
// 条件 x 大概率为假（异常/错误分支）
#define cyber_unlikely(x) (__builtin_expect((x), 0))
#else
// 非 GCC 编译器时降级，不做分支优化
#define cyber_likely(x) (x)
#define cyber_unlikely(x) (x)
#endif

// CPU 缓存行大小（x86 / ARM64 标准均为 64 字节）
// 用于高性能并发编程，避免伪共享、内存对齐
#define CACHELINE_SIZE 64

// =============================================================================
// 类型特征检测宏：编译期判断一个类是否包含指定的成员函数/成员变量
// name：生成的类型检查器名称
// func：要检测的成员名（函数名/变量名）
// 原理：SFINAE 模板匹配机制
// =============================================================================
#define DEFINE_TYPE_TRAIT(name, func)                     \
  template <typename T>                                   \
  struct name {                                           \
    /* 匹配：类 T 存在 func 成员 */                        \
    template <typename Class>                             \
    static constexpr bool Test(decltype(&Class::func)*) { \
      return true;                                        \
    }                                                     \
    /* 兜底：不满足上面条件时匹配这里 */                   \
    template <typename>                                   \
    static constexpr bool Test(...) {                     \
      return false;                                       \
    }                                                     \
                                                          \
    /* 编译期计算结果：是否存在成员 */                     \
    static constexpr bool value = Test<T>(nullptr);       \
  };                                                      \
                                                          \
  /* 静态常量初始化 */                                    \
  template <typename T>                                   \
  constexpr bool name<T>::value;

// =============================================================================
// CPU 轻量级空转指令
// 用于自旋锁、忙等待场景，降低CPU功耗、提升并发效率
// aarch64：yield 指令
// x86：rep nop (pause) 指令
// =============================================================================
inline void cpu_relax() {
#if defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#else
  // ::: "memory" 告诉编译器不要优化内存访问，保持指令顺序
  // asm 指令 : 输出寄存器列表 : 输入寄存器列表 : 破坏寄存器列表
  // "memory" 是一个约束，告诉编译器这个内联汇编可能会修改内存，防止编译器对内存访问进行优化重排序
  asm volatile("rep; nop" ::: "memory");
}

// =============================================================================
// 安全内存分配：封装 malloc
// 分配失败直接抛出 std::bad_alloc 异常，避免空指针风险
// =============================================================================
inline void* CheckedMalloc(size_t size) {
  void* ptr = std::malloc(size);
  if (!ptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

// =============================================================================
// 安全内存分配：封装 calloc
// calloc 会分配内存并自动清零，适用于需要初始化为零的场景
// 分配内存并自动清零，失败抛出异常
// =============================================================================
inline void* CheckedCalloc(size_t num, size_t size) {
  void* ptr = std::calloc(num, size);
  if (!ptr) {
    throw std::bad_alloc();
  }
  return ptr;
}

#endif  // MINICYBER_BASE_MACROS_H_