#ifndef CONTEXT_H
#define CONTEXT_H
#include <cstddef>
#define FIBER_STACK_SIZE 128*1024//128KB,thread stack size default:8MB(ulimit -s)
extern "C" void ctx_swap(void **, void **);

namespace sylar
{
    constexpr size_t REGISTERS_SIZE = 48;
    typedef struct 
    {
        char stack[FIBER_STACK_SIZE];
        char* sp = nullptr;
    }context;
    
    void MakeContext(context *ctx, void(*func)());

    inline void SwapContext(char** src_sp, char** dest_sp){//&sp
        ctx_swap(reinterpret_cast<void**>(src_sp), reinterpret_cast<void**>(dest_sp));
    }
}
#endif
