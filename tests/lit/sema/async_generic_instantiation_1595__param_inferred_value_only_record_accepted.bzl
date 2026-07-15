// Ported from tests/sema/async_generic_instantiation_borrow_1595_test.cpp
//   (BEZEL_TEST async_generic_instantiation_1595, param_inferred_value_only_record_accepted).
//
// ANTI-OVER-REJECT (§30.1 / §9.7 / #1595): an inferred RECORD type argument
// whose only field is a value type (no class/interface-typed field) is NOT
// borrow-bearing (§9.7) — no error, mirroring the declaration-side #1518
// `param_runtime_managed_record_accepted` sibling test.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// v0.190 SB-01 (§28.3): a call whose declared return head is `Task<T>` now
// MINTS an owned task at the call site ("calls mint"), so `t` is a plain-`var`
// owned local; leaving it undischarged would raise the §28.3 auto-drop must-use
// warning (correct, orthogonal to #1595). Discharge it with `using var` so the
// clean-compile control keeps testing ONLY the absence of a spurious borrow
// error — the borrow-check at the instantiation site still runs on this call.
// (`p` is a value-only record, so it carries no disposal obligation of its own.)
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Pair(X: int, Y: int); static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var p := new Pair(1, 2); using var t := F.Work(p); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
