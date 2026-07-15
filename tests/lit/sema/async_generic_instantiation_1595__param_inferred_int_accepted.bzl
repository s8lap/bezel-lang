// Ported from tests/sema/async_generic_instantiation_borrow_1595_test.cpp
//   (BEZEL_TEST async_generic_instantiation_1595, param_inferred_int_accepted).
//
// ANTI-OVER-REJECT (§30.1 / #1595): an inferred `int` type argument is a
// primitive, never borrow-bearing — no error from this rule.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// v0.190 SB-01 (§28.3): a call whose declared return head is `Task<T>` now
// MINTS an owned task at the call site ("calls mint"), so `t` is a plain-`var`
// owned local; leaving it undischarged would raise the §28.3 auto-drop must-use
// warning (correct, orthogonal to #1595). Discharge it with `using var` so the
// clean-compile control keeps testing ONLY the absence of a spurious borrow
// error — the borrow-check at the instantiation site still runs on this call.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int using var t := F.Work(42); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
