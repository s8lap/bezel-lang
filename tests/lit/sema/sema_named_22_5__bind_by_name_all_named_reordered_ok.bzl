// Ported from tests/sema/named_arguments_test.cpp
//   (BEZEL_TEST sema_named_22_5, bind_by_name_all_named_reordered_ok).
//
// §22.5 example: `Log(level := "ERROR", message := "Failed")` — all
// parameters bound by name, in an order different from the declaration.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public method F(a: int, b: int) end F; public method G() this.F(b := 2, a := 1); end G; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
