// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, binding_subclass_over_base_ok_spec_19_5_1203).
//
// §19.5 (#1203): `var d: Dog` over an `Animal` scrutinee is a valid downcast
// type-test binding and must not be rejected.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class Animal end Animal; class Dog: Animal end Dog; class C public static method M(a: Animal): int return (case a of var d: Dog => 1, _ => 0); end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
