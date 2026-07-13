// Ported from tests/sema/alias_transparency_578_test.cpp
//   (BEZEL_TEST sema_alias_transparency_578, function_type_alias_two_params_no_error_spec_27_8).
//
// 6b §27.8 — alias of a two-parameter function type.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; type Combiner := (int, int) -> int; class C public method M() var add: Combiner := (a, b) => a + b; end M; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
