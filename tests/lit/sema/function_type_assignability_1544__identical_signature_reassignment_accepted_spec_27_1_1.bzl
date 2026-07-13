// Ported from tests/sema/function_type_assignability_1544_test.cpp
//   (BEZEL_TEST function_type_assignability_1544, identical_signature_reassignment_accepted_spec_27_1_1).
//
// Control: an identical (int) -> int signature reassignment is accepted —
// the invariance rule rejects a DIFFERENT type, not same-type reuse.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; class C public static method Twice(x: int): int return x * 2; end Twice; public static method Main(): int var f: (int) -> int := Twice; var g: (int) -> int := f; return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
