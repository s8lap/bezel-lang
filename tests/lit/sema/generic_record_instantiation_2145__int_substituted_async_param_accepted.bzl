// ANTI-OVER-REJECT (§30.1 / §9.7, v0.220, issue #2145): a generic record
// instantiated at a VALUE type argument (`Wrap<int>`) — the substituted field
// `V : int` is a value type, no borrow-carrying field, so the record is NOT
// borrow-bearing and the async instantiation stays legal. Pins that the
// substituting field walk classifies by the SUBSTITUTED type, not by "any
// generic record is suspect".
//
// v0.190 SB-01 (§28.3): the `Task<T>`-headed call MINTS an owned task, so the
// result is discharged with `using var` to keep this a diagnostics-clean
// ACCEPT control (mirrors the #1595 value-only-record sibling).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Wrap<T>(V: T); static class F public static async method Work<T>(x: T): Task<T> return x; end Work; end F; class C public static method Main(): int var p := new Wrap<int>(7); using var t := F.Work<Wrap<int>>(p); return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
