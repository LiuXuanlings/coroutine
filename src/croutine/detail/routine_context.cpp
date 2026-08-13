#include "minicyber/croutine/detail/routine_context.h"

#include <cstring>

namespace minicyber {
namespace croutine {

void MakeContext(RoutineContext* ctx, void (*func)()) {
  ctx->sp = ctx->stack + STACK_SIZE - REGISTERS_SIZE - sizeof(void*);
  std::memset(ctx->sp, 0, REGISTERS_SIZE);
  char* sp = ctx->stack + STACK_SIZE - sizeof(void*);
  *reinterpret_cast<void**>(sp) = reinterpret_cast<void*>(func);
}

}  // namespace croutine
}  // namespace minicyber
