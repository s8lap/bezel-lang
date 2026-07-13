// Ported from tests/sema/alias_transparency_578_test.cpp
//   (BEZEL_TEST sema_alias_transparency_578, alias_arg_to_underlying_param_no_error_spec_27_8).
//
// ---------------------------------------------------------------------------
// Control: parameter passing with alias-typed argument already worked (regression).
// §27.8: an alias is transparent in parameter-passing position.
// ---------------------------------------------------------------------------
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; type Meters := double; class C public static method Use(x: double): double => x; public static method M(m: Meters): double => Use(m); end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
