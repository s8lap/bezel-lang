// Control row for #5310 — must ACCEPT both pre-fix and post-fix.
//
// A TYPED null arm, `(null as string?)`, is not a bare `null` literal, so the
// §20 (v0.313) null-arm rule never applies to it: both arms are `string?` and
// the ordinary fold already yields `string?`. This row therefore passed before
// the #5310 fix and must still pass after it — it is the boundary of the fix,
// proving the dropped `!at->nullable` conjunct did not change the path that was
// already correct.
//
// Related: §19.2 (v0.313) case-expression typing, §28.3 (v0.313) arm-meet.

// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var i := 1; var p: string? := "p"; var b := (case i mod 2 of 0 => p, _ => (null as string?)); return (b! + "x").Length; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: warning
// NODIAG-NOT: §
