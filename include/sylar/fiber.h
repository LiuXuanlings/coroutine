#ifndef SYLAR_FIBER_H
#define SYLAR_FIBER_H

#include"sylar/context.h"
#include<functional>
#include<memory>

namespace sylar
{
    class Fiber: public std::enable_shared_from_this<Fiber>{
    public:
        using ptr = std::shared_ptr<Fiber>;
        static thread_local ptr t_fiber;//static means only one shared in this class
        static thread_local ptr t_thread_fiber;//main fiber

        static ptr GetThis();
        Fiber(std::function<void()> cb, int stack_size=FIBER_STACK_SIZE);
        void yield();
        void resume();

        ~Fiber();
    private:
        Fiber();
        //argument of type "void (sylar::Fiber::*)()" is incompatible with parameter of type "void (*)()"
        static void mainFunc();//encapsulate void makecontext(ucontext_t *ucp, void (*func)(), int argc, ...)

    private:
        context m_ctx;//stack variable, don't make it a heap variable
        std::function<void()> m_cb;//RAII
        bool m_is_main;
    };

} // namespace sylar



#endif