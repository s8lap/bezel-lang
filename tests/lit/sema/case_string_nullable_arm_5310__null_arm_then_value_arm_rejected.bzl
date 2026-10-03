// Regression row for #5310 — PIN row (REJECTED pre-fix, ACCEPTED post-fix).
//
// The PRE-FIX diagnostic this row pins away is exactly:
//   error: operator `+` is not defined for type `Object` (§25)
//
// The mirror of `…__value_arm_then_null_arm_rejected.bzl`: the bare `null` arm
// comes FIRST and the `string?` value arm second. Per §20 (v0.313) the null arm
// "target-types to the nullable form of the other branches' common type" — the
// rule is order-independent, so this row must reject pre-fix for the same
// reason and accept post-fix. It exists to pin that the #1952/#5310 typing
// rule reads the whole arm list rather than only its first value arm.
//
// Related: §19.2 (v0.313) case-expression typing, §28.3 (v0.313) arm-meet,
// §28.9 (v0.313) fresh-class disposal.

// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var i := 1; var p: string? := "p"; var b := (case i mod 2 of 0 => null, _ => p); return (b! + "x").Length; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: §
