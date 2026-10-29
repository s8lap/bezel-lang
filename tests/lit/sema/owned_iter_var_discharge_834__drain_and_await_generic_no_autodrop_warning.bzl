// §29.2 / §30.5 / §30.4 / #834 / #1888 (the WhenAllOwned over-reject guard,
// stdlib §2.13): the §29.2 drain-and-await pattern — `for var task in
// arr.Drain() do results.Add(await task) end` over an
// OwningArray<OwningTask<T>> in a GENERIC async method.
//
// The owned iteration variable `task` is discharged by the `await task` inside
// the `Add(owned item)` owned-consuming context: the Pattern B await of a
// drained OwningTask<T> is owned-consuming w.r.t. its operand (§30.5, v0.194),
// and §30.5 lists "an iteration variable from draining an
// `OwningArray<OwningTask<T>>`" as an admitted owned operand — "task discharged
// by the await; no further obligation." The seeding of `task` as owned (the
// #834 fix) made this program reachable; the consume must be recognized so the
// program is ACCEPTED (this is the headline over-rejection guard).
//
// #1888: because the await IS the discharge ("no further obligation"), it must
// ALSO be warning-free — NO spurious §29.2/§28.3 auto-drop must-use warning on
// `task`, exactly like the warning-free plain-local `var t := M(); await t;`.
// (Pre-#1888 the ownership pass emitted the auto-drop warning here AND set the
// body-end auto-drop, which — on a task with no Bezel vtable — double-disposed
// the drained task and crashed the generated code.) This lit test pins the
// warning-free discharge; the runtime exactly-once behavior is covered by
// tests/codegen/drain_owning_task_await_1888_test.cpp.
//
// Clean-compile ACCEPT: bezelc -c must exit zero and emit no auto-drop warning.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static async method WhenAllOwned<T>( owned tasks: OwningArray<OwningTask<T>> ): OwningTask<OwningArray<T>> where T: class using var taskArray := move tasks; var results := new OwningArray<T>(capacity := taskArray.Count); for var task in taskArray.Drain() do results.Add(await task); end; return results; end WhenAllOwned; public static method Main(): int return 0; end Main; end C;

// The await discharges the drained `task` — no auto-drop warning, no error.
// NODIAG-NOT: implicitly disposed at the end of each loop iteration
// NODIAG-NOT: error
