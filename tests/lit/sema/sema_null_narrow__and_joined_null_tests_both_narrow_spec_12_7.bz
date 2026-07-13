// Ported from tests/sema/null_narrowing_test.cpp
//   (BEZEL_TEST sema_null_narrow, and_joined_null_tests_both_narrow_spec_12_7).
//
// §7.4 / §12.7: two null tests joined with `and` both narrow throughout the
// combined true branch.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class A public X: int := 0; end A; class B public Y: int := 0; end B; class C public static method Run(a: A?, b: B?): int if a <> null and b <> null then return a.X + b.Y; end; return -1; end Run; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
