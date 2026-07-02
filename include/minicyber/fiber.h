#ifndef MINICYBER_FIBER_H
#define MINICYBER_FIBER_H

#include "minicyber/context.h"
#include "minicyber/croutine/croutine.h"
#include <functional>
#include <memory>

namespace minicyber
{
    // =====================================================================
    // Fiber <-> CRoutine 兼容桥
    // =====================================================================
    // Fiber 是项目原有的协程实现（state: INIT/EXEC/HOLD/TERM/EXCEPT）。
    // CRoutine 是 CyberRT 风格的新协程（RoutineState: READY/FINISHED/SLEEP/
    // IO_WAIT/DATA_WAIT）。为避免一次性大改破坏现有 Scheduler/IOManager，
    // 此处保留 Fiber 并桥接 RoutineState 语义：
    //
    //   旧 state         新 RoutineState
    //   INIT        ->   READY
    //   EXEC        ->   READY（运行中也视为 READY，CyberRT 语义）
    //   HOLD        ->   SLEEP（让出后默认睡眠；DATA_WAIT/IO_WAIT 由 Yield
    //                       重载显式指定）
    //   TERM        ->   FINISHED
    //   EXCEPT      ->   FINISHED（异常结束也算结束）
    //
    // 通过 Yield(RoutineState) 重载，旧代码可逐步迁移到数据驱动语义，
    // 而现有调用 yield()/resume()/getState() 的代码完全无需改动。
    // =====================================================================
    class Fiber: public std::enable_shared_from_this<Fiber>{
    public:
        using ptr = std::shared_ptr<Fiber>;
        static thread_local ptr t_fiber;//static means only one shared in this class
        static thread_local ptr t_thread_fiber;//main fiber

        enum state{
            INIT,
            EXEC,
            HOLD,
            TERM,
            EXCEPT,
        };

        static ptr GetThis();
        Fiber(std::function<void()> cb, int stack_size=FIBER_STACK_SIZE);
        void yield();
        void resume();
        state getState() const { return m_state; }

        // --- RoutineState 桥接接口（新增，不改原有接口） ---
        // 让出并设置 CyberRT 风格的目标状态（如 DATA_WAIT）
        static void Yield(const RoutineState& state);
        // 获取/设置 RoutineState 视图
        RoutineState GetRoutineState() const;
        void SetRoutineState(const RoutineState& state);

        ~Fiber();
    private:
        Fiber();
        //argument of type "void (minicyber::Fiber::*)()" is incompatible with parameter of type "void (*)()"
        static void mainFunc();//encapsulate void makecontext(ucontext_t *ucp, void (*func)(), int argc, ...)

    private:
        context m_ctx;//stack variable, don't make it a heap variable
        std::function<void()> m_cb;//RAII
        bool m_is_main;
        state m_state;
        // 桥接状态：Yield(RoutineState) 设置此字段，Yield() 无参版本不修改。
        // 当调用 GetRoutineState() 时，若 routine_state_ 已被显式设置过，
        // 优先返回它；否则根据 m_state 做默认映射。
        RoutineState routine_state_ = RoutineState::READY;
    };

} // namespace minicyber



#endif