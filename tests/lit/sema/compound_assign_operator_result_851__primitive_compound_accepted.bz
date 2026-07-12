// Ported from tests/sema/compound_assign_operator_result_851_test.cpp
//   (BEZEL_TEST compound_assign_operator_result_851, primitive_compound_accepted).
//
// Accepted: a primitive compound assignment (no user operator overload).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var x := 5; x += 3; return x; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
