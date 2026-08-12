// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, body_record_auto_props_eligible_spec_9_8).
//
// §9.8 / §9.2: a body record that is auto-hashable AND whose
// auto-properties are all primitive types is serialization-eligible.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; record Pt: ISerializable, IDeserializable public property X: int; public property Y: int; public constructor(x: int, y: int) X := x; Y := y; end constructor; end Pt;

// NODIAG-NOT: error
// NODIAG-NOT: §
