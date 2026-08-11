// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3044 (§17.11 / §14.2 / §3.6 rule 2, v0.242): §17.11's `default(E)`
// escalation covers "`new T()` at such a substitution (§14.2)" and states that
// "a substitution whose `default` is not well-defined — an enum with no
// zero-valued constant included — is rejected at the instantiation site (the
// §3.6 rule 2 pattern)". `new T()` under `where T: new()` at a VALUE
// substitution IS `default(T)`, so an enum with no zero-valued constant does
// not satisfy the constraint, and the diagnostic lands at the INSTANTIATION
// SITE — not inside the generic body, which type-checks under the weakest
// assumption its constraints establish.
//
// `Level` declares 1, 2, 3 — no zero-valued constant.

module M;

enum Level Low := 1, Mid := 2, High := 3; end Level;

static class Utils public static method Make<T>(): int where T: new() var t := new T(); return 1; end Make; end Utils;

class C public static method Main(): int return Utils.Make<Level>(); end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: type argument `Level` does not satisfy the constraint `where T: new()`
// CHECK-SAME: (§14.2)
