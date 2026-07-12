// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/bare_drain_discard_832_test.cpp
//   (BEZEL_TEST bare_drain_discard_832, owned_queue_dequeue_rejected).
//
// OwningQueue.Dequeue() as a bare statement leaks → §21.9 error.

module M; class R public static n: int := 0; destructor R.n := R.n + 1; end destructor; end R; class C public static method Main(): int using var q := new OwningQueue<R>(); q.Enqueue(move (new R())); q.Dequeue(); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: this expression produces an owned value that requires disposal (a non-trivial-dispose class or a move-only owning array); discarding it as a statement leaks the resource — bind it (`using var _ := expression;`), call `.Dispose()`, or transfer it (`move`/`return`)
// CHECK-SAME: (§21.9)
