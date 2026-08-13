#ifndef MINICYBER_CROUTINE_DETAIL_ROUTINE_CONTEXT_H_
#define MINICYBER_CROUTINE_DETAIL_ROUTINE_CONTEXT_H_

#include <cstddef>

extern "C" void ctx_swap(void**, void**);

namespace minicyber {
namespace croutine {

constexpr size_t STACK_SIZE = 2 * 1024 * 1024;
constexpr size_t REGISTERS_SIZE = 48;

struct RoutineContext {
  char stack[STACK_SIZE];
  char* sp = nullptr;
};

void MakeContext(RoutineContext* ctx, void (*func)());

inline void SwapContext(char** src_sp, char** dest_sp) {
  ctx_swap(reinterpret_cast<void**>(src_sp), reinterpret_cast<void**>(dest_sp));
}

}  // namespace croutine
}  // namespace minicyber

#endif  // MINICYBER_CROUTINE_DETAIL_ROUTINE_CONTEXT_H_
