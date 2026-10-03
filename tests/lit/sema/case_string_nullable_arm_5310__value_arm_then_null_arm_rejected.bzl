// Regression row for #5310 — PIN row (REJECTED pre-fix, ACCEPTED post-fix).
//
// The PRE-FIX diagnostic this row pins away is exactly:
//   error: operator `+` is not defined for type `Object` (§25)
//
// §20 (v0.313): a bare `null` arm "imposes no constraint of its own … it
// target-types to the nullable form of the other branches' common type".
// §19.2 (v0.313) ties an expression-position `case … of` result to that same
// rule. The other arm here is a `string?`, so the nullable form of its common
// type is `string?` — NOT `Object?`.
//
// Pre-fix, ExprTC::typeCase's #1952 null-arm rule required every non-null value
// arm to be a NON-nullable `string`, so this `string?` arm fell through to the
// general fold and the expression was typed `Object?`. The `b!` below then
// produced an `Object`, and `+` has no `Object` overload (§8.1/§8.4/§25), so
// the program was rejected. Post-fix it types `string?`, `b!` narrows to
// `string` (§7.8), and the program is well-typed.
//
// Related: §28.3 (v0.313) arm-meet, §28.9 (v0.313) fresh-class disposal —
// on the pre-fix `Object?` typing both codegen sides that key on a string-typed
// result declined, and the arm's reference leaked on every evaluation.

// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var i := 1; var p: string? := "p"; var b := (case i mod 2 of 0 => p, _ => null); return (b! + "x").Length; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: §
