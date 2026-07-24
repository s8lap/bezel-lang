// §22.8 (v0.226) single-assignment of readonly fields (#2187) — ACCEPT control.
//
// A `static readonly` field assigned exactly once in the static constructor is
// the legitimate single-assignment shape; the static-ctor once-check must not
// over-reject it.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static readonly _s: string?; static constructor() _s := "ok"; end constructor; public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §22.8
