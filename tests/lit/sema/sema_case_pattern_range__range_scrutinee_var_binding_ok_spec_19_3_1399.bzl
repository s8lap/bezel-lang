// Ported from tests/sema/case_pattern_literal_range_1153_test.cpp
//   (BEZEL_TEST sema_case_pattern_range, range_scrutinee_var_binding_ok_spec_19_3_1399).
//
// §19.3 (#1399): `var x` (unconstrained binding) over a Range scrutinee is
// valid — binds the whole Range value; must not be rejected. Pin it.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var r := 1..10; return case r of var x => 0 end; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
