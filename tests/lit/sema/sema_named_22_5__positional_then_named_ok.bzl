// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, positional_then_named_ok).
//
// §22.5: positional arguments fill in order, a named argument binds the
// rest — `F(1, c := 3, b := 2)`-style mix of positional then named.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(a: int, b: int, c: int) end F; public method G() this.F(1, c := 3, b := 2); end G; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
