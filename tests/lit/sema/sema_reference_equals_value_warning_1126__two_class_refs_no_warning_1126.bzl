// Ported from tests/sema/reference_equals_value_warning_1126_test.cpp
//   (BEZEL_TEST sema_reference_equals_value_warning_1126, two_class_refs_no_warning_1126).
//
// §8.4 / §14.2: two class (reference-type) operands — no warning.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class N public v: int := 0; end N; class C public method M(a: N, b: N): bool => Object.ReferenceEquals(a, b); end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
