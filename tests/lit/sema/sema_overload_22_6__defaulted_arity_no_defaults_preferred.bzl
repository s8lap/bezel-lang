// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, defaulted_arity_no_defaults_preferred).
//
// §22.6 "Defaulted-arity tie-break" (v0.181, #1561) — a candidate applicable
// WITHOUT filling any default is preferred over one that fills one or more,
// provided it is better-or-equal at every supplied position. `Log(int, int
// := 0)` is placed FIRST (so a first-wins/arity fallback would pick it, and
// pre-fix the two were simply reported ambiguous) and returns bool;
// `Log(int)` is placed second and returns int. `this.Log(5)` binds to `var
// x: int`, which type-checks only if the no-defaults candidate — `Log(int)`
// per the spec's own `M(5)` example — was selected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Foo public method Log(n: int, m: int := 0): bool => true; public method Log(n: int): int => n; public method Use() var x: int := this.Log(5); end Use; end Foo;

// NODIAG-NOT: error
// NODIAG-NOT: §
