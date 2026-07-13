// Ported from tests/sema/local_const_eval_fault_1688_test.cpp
//   (BEZEL_TEST local_const_eval_fault_1688, runtime_division_by_zero_not_a_compile_error).
//
// §17.14 only governs a compile-time-constant expression. A `/` between two
// ordinary (non-const) `var` locals is a runtime expression — division by
// zero there is (and must remain) a runtime DivideByZeroException, not a
// compile error. This pins that the fix's fold-and-check gate is scoped to
// `const` initializers and does not reach into ordinary expressions.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var x := 5; var y := 0; var z := x / y; return z; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
