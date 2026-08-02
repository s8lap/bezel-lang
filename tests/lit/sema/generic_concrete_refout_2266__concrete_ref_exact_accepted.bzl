// §22.3 / §14.5 (spec v0.228) / #2266 over-rejection control: the same generic
// method with a CONCRETELY-typed ref parameter, called with an exactly-matching
// `int` location. The new per-parameter check must accept it — this is the
// case the fix newly reaches, so it is the one most at risk of over-rejecting.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; static class U public static method Store<T>(v: T, ref slot: int) slot := 1; end Store; end U; class C public static method Main(): int var n: int := 0; U.Store<int>(5, ref n); return n; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
