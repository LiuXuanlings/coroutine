.global ctx_swap

ctx_swap:
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushq %rbx
    pushq %rbp
    movq %rsp, (%rdi)

    movq (%rsi), %rsp
    popq %rbp
    popq %rbx 
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    ret