// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, positional_after_named_diagnosed).
//
// §22.5: "Named arguments may appear ... after positional arguments" — so a
// positional argument that follows a named one is rejected.

module M; class C public method F(a: int, b: int) end F; public method G() this.F(a := 1, 2); end G; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call to `F`: a positional argument may not follow a named argument
// CHECK-SAME: (§22.5 — named arguments come after all positional arguments)
