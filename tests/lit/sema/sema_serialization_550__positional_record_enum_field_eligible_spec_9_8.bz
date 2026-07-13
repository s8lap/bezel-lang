// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, positional_record_enum_field_eligible_spec_9_8).
//
// §9.8: an enum field is an eligible component type.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Status Active, Inactive; end Status; record Item(Name: string, St: Status): ISerializable, IDeserializable;

// NODIAG-NOT: error
// NODIAG-NOT: §
