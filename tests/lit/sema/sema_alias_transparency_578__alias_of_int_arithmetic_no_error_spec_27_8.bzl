// Ported from tests/sema/alias_transparency_578_test.cpp
//   (BEZEL_TEST sema_alias_transparency_578, alias_of_int_arithmetic_no_error_spec_27_8).
//
// 6a §27.8 — alias of int: arithmetic operators (+, -, *, /) work transparently.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; type Count := int; class C public static method M(a: Count, b: Count): int var s := a + b; var d := a - b; var p := a * b; var q := a / b; return s; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
