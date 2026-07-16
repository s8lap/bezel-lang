// Regression for #1873 — NO-OVER-REJECTION control for the post-erasure
// overload-collapse rule (§22.6 / §28.1 v0.196).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.
//
// Same generic shape as the collapse reject, but the two `Store` overloads
// differ in the DECLARED PARAMETER TYPE (`T` vs `int`), not by an `owned`
// marker.  A real type difference distinguishes overloads (§22.6), so the pair
// is a legal overload set — the owned-collapse rule must NOT over-reject
// genuinely distinct generic overloads.  (`T = int` would collide the two only
// through the type argument, which is not a declaration-time clash — the same
// generic-overload admissibility every generic type relies on.)
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Box<T> public method Store(x: T) _ := 1; end Store; public method Store(x: int) _ := 2; end Store; end Box;

// NODIAG-NOT: error
// NODIAG-NOT: §
