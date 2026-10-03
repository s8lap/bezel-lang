// Control row for #5310 — must ACCEPT both pre-fix and post-fix.
//
// The `if`-expression twin of the pinned shape, `(if c then p else null)` with
// `p: string?`. Per §20 (v0.313) this already had type `string?` before #5310:
// ExprTC::typeConditional applies the null-arm rule for ALL types because the
// if-expression lowering path differs. This row must be unchanged by the fix —
// it is the control that shows the defect was specific to the `case` path's
// string-scoped rule, not to the null-arm rule itself.
//
// Related: §19.2 (v0.313) case-expression typing, §28.3 (v0.313) arm-meet.

// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var i := 1; var p: string? := "p"; var b := (if i mod 2 = 0 then p else null); return (b! + "x").Length; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: §
