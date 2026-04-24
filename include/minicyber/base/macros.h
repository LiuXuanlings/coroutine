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
// name：生成的类型检查器名称（比如 HasFoo）
// func：要检测的成员名（比如 foo）
// 原理：SFINAE (Substitution Failure Is Not An Error)模板匹配机制
// =============================================================================
#define DEFINE_TYPE_TRAIT(name, func)                     \
  template <typename T>                                   \
  struct name {                                           \
    /* -------------------------------------------------  \
       重载 1：检测 T 是否存在 func 成员                      \
       关键技巧：decltype(&Class::func) 只在 Class 有 func 时才合法 \
       decltype 是 C++11 引入的编译时类型推导工具              \
       ------------------------------------------------- */ \
    template <typename Class>                             \
    static constexpr bool Test(decltype(&Class::func)*) { \
      /* 如果能走到这里，说明 decltype(&Class::func) 是合法的，\
         即 Class 确实有 func 这个成员（函数或变量） */        \
      return true;                                        \
    }                                                     \
    /* -------------------------------------------------  \
       重载 2：万能兜底（... 表示"任意参数"）                  \
       当重载 1 的替换失败时（Class 没有 func），编译器不报错，  \
       而是退而求其次匹配这个版本                             \
       ------------------------------------------------- */ \
    template <typename>                                   \
    static constexpr bool Test(...) {                     \
      return false;                                       \
    }                                                     \
                                                          \
    /* -------------------------------------------------  \
       编译期计算结果：                                      \
       Test<T>(nullptr) 会尝试把 T 代入两个重载               \
       - 如果 T 有 func：匹配重载1，返回 true                 \
       - 如果 T 没有 func：重载1替换失败，匹配重载2，返回 false  \
       - nullptr 只是为了满足参数列表，因为重载1需要一个指针参数  \
       ------------------------------------------------- */ \
    static constexpr bool value = Test<T>(nullptr);       \
  };                                                      \
                                                          \
  /* ---------------------------------------------------  \
     C++11/14 要求：类内的 static constexpr 成员如果是 ODR-used("One Definition Rule used") \
     ODR-used = 这个变量在程序运行时需要占用内存地址（不只是编译期常量值）。 \
     那么就必须在类外提供一个定义（即使它已经在类内初始化了）。 \
      C++17 之后允许 inline constexpr，类内初始化后不需要再定义了。 \
     --------------------------------------------------- */ \
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