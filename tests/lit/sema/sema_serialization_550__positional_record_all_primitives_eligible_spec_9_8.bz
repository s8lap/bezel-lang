// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, positional_record_all_primitives_eligible_spec_9_8).
//
// §9.8: all primitive types are eligible component types.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Primitives(B: bool, C: char, I: int, L: long, F: float, D: double): ISerializable;

// NODIAG-NOT: error
// NODIAG-NOT: §
