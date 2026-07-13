// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, record_positional_no_explicit_ctor_accepted_spec_9_8_v0182).
//
// §9.8 v0.182: a purely positional record (§9.1) has no explicit constructor
// (the auto-generated primary constructor is not "explicit") — the
// constructor-selection rule does not apply, no error.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Point(X: int, Y: int): ISerializable, IDeserializable;

// NODIAG-NOT: error
// NODIAG-NOT: §
