// Ported from tests/sema/range_literal_step_zero_1628_test.cpp
//   (BEZEL_TEST sema_range_literal_step_zero_1628, runtime_computed_step_zero_not_flagged_at_compile_time_spec_16_4).
//
// §16.4 control: a COMPUTED (non-constant) step of zero is a separate,
// runtime concern (#1623 — ArgumentException at GetEnumerator()), NOT a
// compile error. foldIntConstExprTC cannot fold a plain local-variable
// read, so the literal-step-zero check here must stay silent and let the
// program compile; the codegen-side runtime guard is exercised by
// tests/codegen/for_range_runtime_step_test.cpp.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Main(): int var z: int := 0; var sum: int := 0; for var i in 0..10 step z do sum := sum + i; end; return sum; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
