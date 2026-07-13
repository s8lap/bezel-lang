// Ported from tests/sema/overload_resolution_test.cpp
//   (BEZEL_TEST sema_overload_22_6, defaulted_arity_full_arity_call_unaffected).
//
// §22.6 defaulted-arity tie-break — supplying the optional argument makes
// only the two-parameter form applicable (arity alone excludes `Log(int)`),
// so `Log(5, 7)` is unaffected by the tie-break and still resolves cleanly
// to the two-parameter overload.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Foo public method Log(n: int): bool => true; public method Log(n: int, m: int := 0): int => n + m; public method Use() var x: int := this.Log(5, 7); end Use; end Foo;

// NODIAG-NOT: error
// NODIAG-NOT: §
