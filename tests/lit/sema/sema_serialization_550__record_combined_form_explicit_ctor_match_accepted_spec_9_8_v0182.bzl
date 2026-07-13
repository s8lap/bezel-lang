// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, record_combined_form_explicit_ctor_match_accepted_spec_9_8_v0182).
//
// §9.8 v0.182 (via §9.3 combined form): a combined-form record's positional
// fields count as storage components alongside body auto-properties for the
// SAME constructor-selection rule — an explicit constructor bijection-matching
// the positional fields by name is accepted.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Vec2(X: int, Y: int): ISerializable, IDeserializable public constructor(x: int, y: int) X := x + 100; Y := y + 100; end constructor; end Vec2;

// NODIAG-NOT: error
// NODIAG-NOT: §
