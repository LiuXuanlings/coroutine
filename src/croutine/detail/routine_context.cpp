#include "minicyber/croutine/detail/routine_context.h"

#include <cstring>

namespace minicyber {
namespace croutine {

void MakeContext(RoutineContext* ctx, void (*func)()) {
  ctx->sp = ctx->stack + STACK_SIZE - 2 * sizeof(void*) - REGISTERS_SIZE;
  std::memset(ctx->sp, 0, REGISTERS_SIZE);
  char* sp = ctx->stack + STACK_SIZE - 2 * sizeof(void*);
  *reinterpret_cast<void**>(sp) = reinterpret_cast<void*>(func);
}

}  // namespace croutine
}  // namespace minicyber
