// §22.8 (v0.226) single-assignment of readonly fields (#2187) — ACCEPT control.
//
// A branch that assigns `_f` and then diverges (`return`) reaches no join, so
// its assignment cannot collide with a later assignment on the only live path.
// `if c then _f := "a"; return; end; _f := "b";` assigns `_f` exactly once on
// each of the two exit paths — accepted (the union-over-LIVE-branches join
// excludes the terminated `then` arm, mirroring the field-DA #1753 fix).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public readonly _f: string?; public constructor(c: bool) if c then _f := "a"; return; end; _f := "b"; end constructor; destructor end destructor; public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §22.8
