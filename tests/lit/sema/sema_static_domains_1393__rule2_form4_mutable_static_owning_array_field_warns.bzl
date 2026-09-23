// §22.12 rule 2 form (iv) (v0.221 ruling / #1393, spec v0.228): a mutable
// owning-inline-array static FIELD (`[N owned]T?` / `[N owned]T`, §15.1.1)
// is admitted with a declaration-site warning — the admission §22.13's
// "Disposal of static owned fields" / §11.7 already made, named inside
// rule 2 here.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero, but it DOES emit a
// diagnostic — pinned below.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class Cell public n: int := 0; end Cell; class App public static _pool: [2 owned]Cell?; public static method Main(): int => 0; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `_pool` has owning-inline-array type `[2 owned]Cell?` — admitted with a declaration-site warning (§22.12 rule 2 form (iv)): the array's owned instances are reachable from every domain and its slots are mutable; confine every slot read/write, `move arr[j]`, and whole-field reassignment to one domain, or restructure behind a `Locked<T>` registry
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
