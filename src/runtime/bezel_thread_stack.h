#ifndef BEZEL_THREAD_STACK_H
#define BEZEL_THREAD_STACK_H
// bezel_thread_stack.h — the stack size of every thread a Bezel program runs.
//
// The runtime (bezel_runtime_internal.hpp, BzThread) creates each of its
// threads — the scheduler pool that runs Domain.Spawn workers and async
// continuations, and the Task.Delay timer — with exactly this stack size.
//
// WHY. A thread's default stack is a platform choice, not a language one, and
// the platforms disagree: glibc takes RLIMIT_STACK (8 MiB on a stock Linux
// box), musl uses a fixed 128 KiB. Measured (#6695, #6654 spike): a 20000-deep
// recursion on a Domain.Spawn worker runs under glibc and segfaults under
// musl. 8 MiB is glibc's usual default, so a program behaves the same in the
// hosted (glibc) and the self-contained (musl) build; it is also musl's cap on
// a PT_GNU_STACK-requested default (DEFAULT_STACK_MAX).
//
// Plain C, and it carries no declarations, only the number, so any consumer
// that must agree with the runtime can include it.
#define BZ_THREAD_STACK_SIZE (8u * 1024u * 1024u)

#endif /* BEZEL_THREAD_STACK_H */
