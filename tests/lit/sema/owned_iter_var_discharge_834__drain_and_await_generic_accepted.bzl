// Ported from tests/sema/owned_iter_var_discharge_834_test.cpp
//   (BEZEL_TEST owned_iter_var_discharge_834, drain_and_await_generic_accepted).
//
// §29.2 / §30.5 / §30.4 / #834 (the WhenAllOwned over-reject guard, stdlib
// §2.13): the §29.2 drain-and-await pattern — `for var task in arr.Drain() do
// results.Add(await task) end` over an OwningArray<OwningTask<T>> in a GENERIC
// async method. The owned iteration variable `task` is discharged by the
// `await task` inside the `Add(owned item)` owned-consuming context: awaiting
// an `OwningTask<...>` consumes the single-consumer task (§30.5/§30.4). This
// must be ACCEPTED. The seeding of `task` as owned (the #834 fix) made this
// program reachable; the consume of `await task` in an argument position (and
// for a generic OwningTask<T> whose result class kind awaitTaskKind cannot
// prove) must be recognized, else this conformance-clean program is wrongly
// rejected. This is the headline regression to guard against over-rejection.
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is legal), but it DOES emit a diagnostic — pinned below so the warning itself stays observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class C public static async method WhenAllOwned<T>( owned tasks: OwningArray<OwningTask<T>> ): OwningTask<OwningArray<T>> where T: class using var taskArray := tasks; var results := new OwningArray<T>(capacity := taskArray.Count); for var task in taskArray.Drain() do results.Add(await task); end; return results; end WhenAllOwned; public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: owned value in `task` is implicitly disposed at the end of each loop iteration
// CHECK-SAME: (§29.2/§28.3 auto-drop)
