// §22.8 (v0.226) single-assignment of readonly fields on RECORDS (#2207) — ACCEPT control.
//
// Conditional single-assignment across mutually-exclusive `if`/`else` branches
// on a record is legal: `_f` is assigned exactly ONCE on every construction
// path (once per branch, never twice on one path). The over-reject failure
// mode of DA / construction-window checks would wrongly flag the second
// branch's assignment as a re-assignment; the per-branch reset + union join
// (#2187) must keep it accepted when applied to records (#2207).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record R public readonly _f: string?; public constructor(c: bool) if c then _f := "a"; else _f := "b"; end; end constructor; public static method Main(): int return 0; end Main; end R;

// NODIAG-NOT: error
// NODIAG-NOT: §22.8