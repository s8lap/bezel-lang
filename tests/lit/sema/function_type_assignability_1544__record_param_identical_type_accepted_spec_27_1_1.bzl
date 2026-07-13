// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, record_param_identical_type_accepted_spec_27_1_1).
//
// Control: an IDENTICAL record-typed signature is accepted (invariance
// rejects a DIFFERENT record, not the reassignment itself).
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record R1(X: int); class C public static method TakeR1(r: R1): int return r.X; end TakeR1; public static method Main(): int var f: (R1) -> int := TakeR1; var g: (R1) -> int := f; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
