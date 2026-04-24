#include"minicyber/context.h"
#include"minicyber/fiber.h"
#include<cstring>

namespace minicyber{
    void MakeContext(context *ctx, void(*func)()){
        ctx->sp = ctx->stack + FIBER_STACK_SIZE - REGISTERS_SIZE - sizeof(void *);
        memset(ctx->sp, 0, REGISTERS_SIZE);
        char *sp = ctx->stack + FIBER_STACK_SIZE - sizeof(void *);

        *reinterpret_cast<void**>(sp) = reinterpret_cast<void *> (func);
    }
}