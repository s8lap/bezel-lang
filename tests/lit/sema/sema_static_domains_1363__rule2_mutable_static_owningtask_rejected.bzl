// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/static_field_domains_1363_test.cpp
//   (BEZEL_TEST sema_static_domains_1363, rule2_mutable_static_owningtask_rejected).
//
// (23) §22.12 rule 2: `OwningTask<T>?` is NOT admitted (form (ii) is `Task?` /
// `Task<T>?` only — an owning task is single-await and would strand its
// result as a shared cache).  A mutable static OwningTask<T>? is rejected.

module M; class Doc public X: int := 0; end Doc; class App private static _doc: OwningTask<Doc>? := null; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: mutable static field `_doc` has type `OwningTask`, which §22.12 rule 2 does not admit as a mutable static; a mutable static of class or interface type must be an internally-synchronized handle (Mutex, Semaphore, Atomic<T>, AtomicInteger<T>, ChannelSender<T> — form (i); Channel<T> is excluded, publish a ChannelSender<T> instead) or a `Task?` / `Task<T>?` lazy cache (form (ii); OwningTask<T>? is not admitted)
// CHECK-SAME: (§22.12 rule 2, v0.181, #1556)
