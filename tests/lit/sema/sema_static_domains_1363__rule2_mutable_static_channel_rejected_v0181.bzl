// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule2_mutable_static_channel_rejected_v0181).
//
// (7) §22.12 rule 2 (v0.181, #1556): Channel<int> is EXCLUDED from the
// synchronized set — a mutable static Channel<T> is now REJECTED.  (This test
// asserted admission under v0.163, when Channel was in the form-(i) set; the
// v0.181 revision removes it — the channel object is the single-consumer
// receiving end (§30.15) and a process-global receiving end would readmit the
// deferred multi-consumer receive (stdlib §6.9).  Publish a ChannelSender<T>
// static instead.)  Legitimate test update for a spec revision, per the
// milestone workflow.

module M; class App public static _ch: Channel<int>; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_ch` has type `Channel`, which §22.12 rule 2 does not admit as a mutable static; a mutable static of class or interface type must be an internally-synchronized handle (Mutex, Semaphore, Atomic<T>, AtomicInteger<T>, ChannelSender<T> — form (i); Channel<T> is excluded, publish a ChannelSender<T> instead) or a `Task?` / `Task<T>?` lazy cache (form (ii); OwningTask<T>? is not admitted)
// CHECK-SAME: (§22.12 rule 2, v0.181, #1556)
