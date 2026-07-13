// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, enum_iserializable_eligible_spec_9_8).
//
// §9.8: an enum declaring ISerializable is always eligible — no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Priority: ISerializable Low, Medium, High; end Priority;

// NODIAG-NOT: error
// NODIAG-NOT: §
