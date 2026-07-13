// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, enum_ideserializable_eligible_spec_9_8).
//
// §9.8: an enum declaring IDeserializable is always eligible — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Color: IDeserializable Red, Green, Blue; end Color;

// NODIAG-NOT: error
// NODIAG-NOT: §
