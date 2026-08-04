// Ported from tests/sema/async_generic_instantiation_borrow_1595_test.cpp
//   (BEZEL_TEST async_generic_instantiation_1595, nonasync_generic_method_not_checked).
//
// §30.1 / #1595 (scope guard): the instantiation-site check is `async`-only,
// exactly like the declaration-site check (#1518
// `nonasync_span_param_accepted`) — a SYNCHRONOUS generic method's borrow
// captured by a value argument is bounded by the ordinary call frame
// (§28.3) and needs no state-machine-frame rule at all. Same borrow-bearing
// type argument as the rejected async case above, on the sync twin: no
// error.
//
// ACCEPT-SILENTLY control: bezelc -c must exit zero (the program is legal)
// AND must emit no diagnostic at all.
//
// This fixture previously pinned the §28.3 auto-drop warning here. That
// expectation encoded the #2654 bug: the class bound below is TRIVIAL-DISPOSE
// (no destructor, no `owned` field, no owning-inline-array field, trivial
// base), and spec §28.9 (v0.237) "Trivial-dispose classes" states that "a
// `new` expression of a trivial-dispose class type whose result is not stored
// in an `owned` location or `using var` does *not* produce the standard
// ownership-discharge warning. (There is nothing to discharge.)" The
// CHECK-NOT below now pins that exemption; this file's own subject (above) is
// unchanged, and the program text is untouched.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --allow-empty %s

module M; class Res end Res; record Holder(R: Res); static class G public static method Work<T>(x: T): T return x; end Work; end G; class C public static method Main(): int var res := new Res(); var h := new Holder(res); var t := G.Work(h); return 0; end Main; end C;

// CHECK-NOT: §28.3 auto-drop
