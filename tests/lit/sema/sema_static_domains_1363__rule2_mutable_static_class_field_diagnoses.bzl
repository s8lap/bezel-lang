// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule2_mutable_static_class_field_diagnoses).
//
// (1) §22.12 rule 2: a mutable static field of a user class type is an error.
// Probe: prior to fix, this was accepted silently.

module M; class Cfg public X: int := 0; end Cfg; class App public static _cfg: Cfg := new Cfg(); public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_cfg` has type `Cfg`, which §22.12 rule 2 does not admit as a mutable static; a mutable static of class or interface type must be an internally-synchronized handle (Mutex, Semaphore, Atomic<T>, AtomicInteger<T>, ChannelSender<T> — form (i); Channel<T> is excluded, publish a ChannelSender<T> instead) or a `Task?` / `Task<T>?` lazy cache (form (ii); OwningTask<T>? is not admitted)
// CHECK-SAME: (§22.12 rule 2, v0.181, #1556)
