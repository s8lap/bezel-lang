// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule2_mutable_static_autoprop_class_diagnoses).
//
// (17) §22.12 rule 2: a mutable static auto-property of user class type is
// an error.  Auto-properties with Auto bodyKind are storage-backed — they
// are mutable shared state.

module M; class Registry public X: int := 0; end Registry; class App public static property Instance: Registry?; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static property `Instance` has type `Registry`, which §22.12 rule 2 does not admit as a mutable static; a mutable static of class or interface type must be an internally-synchronized handle (Mutex, Semaphore, Atomic<T>, AtomicInteger<T>, ChannelSender<T> — form (i); Channel<T> is excluded, publish a ChannelSender<T> instead) or a `Task?` / `Task<T>?` lazy cache (form (ii); OwningTask<T>? is not admitted)
// CHECK-SAME: (§22.12 rule 2, v0.181, #1556)
