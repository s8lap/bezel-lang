// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/ownership/borrow_into_bare_field_1082_test.cpp
//   (BEZEL_TEST borrow_into_bare_field_1082, bare_static_field_borrow_rejected).
//
// A bare unqualified STATIC field borrow store is rejected too.

module M; class Cfg public _v: int := 0; public constructor(v: int) _v := v; end constructor; end Cfg; class C public static _cfg: Cfg?; public static method Set() using var cfg := new Cfg(99); _cfg := cfg; end Set; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_cfg` has type `Cfg`, which §22.12 rule 2 does not admit as a mutable static; a mutable static of class or interface type must be an internally-synchronized handle (Mutex, Semaphore, Atomic<T>, AtomicInteger<T>, ChannelSender<T> — form (i); Channel<T> is excluded, publish a ChannelSender<T> instead) or a `Task?` / `Task<T>?` lazy cache (form (ii); OwningTask<T>? is not admitted)
// CHECK-SAME: (§22.12 rule 2, v0.181, #1556)
