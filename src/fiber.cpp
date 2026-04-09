#include"sylar/fiber.h"
#include"sylar/context.h"
#include <exception>
#include <new>
#include<functional>
#include<iostream>
#include<stdio.h>

namespace sylar{
    // static thread_local: DECLARE in .h, DEFINE in .cpp (no static)
    thread_local Fiber::ptr Fiber::t_fiber=nullptr;//symbol names(Fiber::t_fiber) must match exactly 
    thread_local Fiber::ptr Fiber::t_thread_fiber=nullptr;
}


sylar::Fiber::Fiber(){//main fiber share stack with thread
    m_is_main = true;
    m_state = INIT;
}

sylar::Fiber::~Fiber(){
}

sylar::Fiber::ptr sylar::Fiber::GetThis(){
    if(t_fiber!=nullptr) return t_fiber; 
    ptr fiber(new Fiber());
    t_thread_fiber = fiber;
    t_fiber = fiber;
    return t_fiber;
}

/*
    * Root cause (Memory Leak):
    * When the fiber finishes executing its callback (`m_cb`), it must yield back to the 
    * main thread one last time. If we simply call `GetThis()->yield()`, `GetThis()` 
    * generates a temporary `std::shared_ptr` on THIS fiber's stack.
    * 
    * Since this fiber has finished its job, it will NEVER be resumed again. 
    * Because it is never resumed, the execution never reaches the end of the statement, 
    * and the temporary `shared_ptr` on this "frozen" stack NEVER destructs. 
    * This keeps the Fiber's reference count > 0 forever, perfectly leaking the Fiber 
    * object and its 128KB allocated stack.
    *
    * Fix:
    * 1. Extract the `shared_ptr` into a local variable `cur`.
    * 2. Clear `cur->m_cb` to release any captured `shared_ptr`s inside the callback closure.
    * 3. Extract the raw pointer (`Fiber* raw_ptr`).
    * 4. Call `cur.reset()` to forcefully destroy the `shared_ptr` on this stack BEFORE yielding.
    * 5. Context-switch back to main using `raw_ptr->yield()`.
    * 
    * By the time this fiber is suspended forever, its stack is 100% clean of self-referencing 
    * smart pointers. When the main thread drops the fiber, it will be safely destroyed.
    */
void sylar::Fiber::mainFunc() {
    sylar::Fiber::ptr cur = GetThis();
    cur->m_state = EXEC;

    try {
        cur->m_cb();
        cur->m_cb = nullptr;
        cur->m_state = TERM;
    } catch (...) {
        cur->m_cb = nullptr;
        cur->m_state = EXCEPT;
    }

    // Extract raw pointer to call yield later without creating temporary shared_ptrs
    sylar::Fiber* raw_ptr = cur.get();

    // Manually destroy the shared_ptr on this fiber's stack.
    // The reference count drops by 1. (The main thread still holds a copy, so it won't die yet).
    cur.reset();

    // Context switch back to the main thread.
    // This fiber will freeze here forever, but its stack is now clean!
    raw_ptr->yield();
}

sylar::Fiber::Fiber(std::function<void()> cb, int stack_size){
    m_is_main = false;
    m_cb = cb;
    m_state = INIT;
    /*
    * Root cause: ucontext_t was not initialized with getcontext()
    * uc_mcontext (register state) is zero → swapcontext crashes on invalid RIP
    * Fix: Call getcontext() first to initialize ucontext_t before setting stack/makecontext
    */
    MakeContext(&m_ctx,Fiber::mainFunc);
}

void sylar::Fiber::yield(){
    t_fiber = t_thread_fiber;
    if(this->m_state == EXEC){
        this->m_state = HOLD;
    }
    if(!m_is_main){
        SwapContext(reinterpret_cast<char**>(&this->m_ctx.sp), reinterpret_cast<char**>(&t_thread_fiber->m_ctx.sp));
    }
}

void sylar::Fiber::resume(){
    /*
     * Root cause:
     * Using `static_cast<shared_ptr>(this)` creates a BRAND NEW Control Block (CB) 
     * upon every `resume()`. Temporary `shared_ptr`s returned by `GetThis()` delay 
     * the destruction, perfectly masking the bug until the stack unwinds.
     *
     * [ Timeline of the Delayed Use-After-Free / Double Free ]
     *
     * MAIN THREAD                  FIBER THREAD (mainFunc & run_in_fiber)
     * -----------------------------------------------------------------------------------------
     * resume() 1st time
     * t_fiber = NEW CB1 ---------> mainFunc: GetThis()->m_cb(); 
     *                              [!] Creates Temp T1 holding CB1.
     *                                |
     *                                +-> run_in_fiber: GetThis()->yield(); 
     *                                    [!] Creates Temp T2 holding CB1.
     *                                    |
     * <------- yield() 1st time ---------+ (t_fiber clears, but T1 & T2 keep CB1 alive)
     * 
     * resume() 2nd time
     * t_fiber = NEW CB2 ---------> (Fiber wakes up)
     *                              Statement ends -> T2 destructs (CB1 ref-1).
     *                                |
     *                                +-> run_in_fiber: GetThis()->yield(); 
     *                                    [!] Creates Temp T3 holding CB2.
     *                                    |
     * <------- yield() 2nd time ---------+ (t_fiber clears, but T3 keeps CB2 alive)
     * 
     * resume() 3rd time
     * t_fiber = NEW CB3 ---------> (Fiber wakes up)
     *                              Statement ends -> T3 destructs.
     *                              [!] CB2 ref_count == 0 ---> 1st DELETE (Memory Freed!)
     *                                |
     *                              run_in_fiber ends, returns to mainFunc.
     *                              m_cb() statement ends -> T1 destructs.
     *                              [!] CB1 ref_count == 0 ---> 2nd DELETE (ASan CRASH!)
     * -----------------------------------------------------------------------------------------
     *
     * Fix:
     * Replaced `static_cast<std::shared_ptr<Fiber>>(this)` with `shared_from_this()`.
     * `shared_from_this()` correctly looks up the ORIGINAL Control Block (created in main)
     * and safely increments its ref_count. All `shared_ptr`s now share the same manager.
     *
     * Lesson:
     * 1. NEVER construct a new `shared_ptr` from `this` if the object is already managed.
     * 2. Anonymous temporaries (like `GetThis()->...`) extend object lifetimes until the 
     *    end of the statement. They can deeply hide fatal memory corruptions by delaying 
     *    the execution of the destructor.
     */
    //t_fiber = static_cast<std::shared_ptr<Fiber>>(this);
    t_fiber = shared_from_this();
    this->m_state = EXEC;
    if(!m_is_main){
        SwapContext(reinterpret_cast<char**>(&t_thread_fiber->m_ctx.sp), reinterpret_cast<char**>(&m_ctx.sp));
    }
}