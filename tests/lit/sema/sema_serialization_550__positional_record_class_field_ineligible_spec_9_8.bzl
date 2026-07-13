// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, positional_record_class_field_ineligible_spec_9_8).
//
// §9.8: a positional record with a CLASS-typed field (not a serializable class)
// is NOT serialization-eligible. A class without `: ISerializable`/`: IDeserializable`
// is not a "serialization-eligible synthesized class" (§9.8, type-driven v0.141).

module M; class C end C; record R(X: int, Obj: C): ISerializable;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R` declares `ISerializable`/`IDeserializable` but is not serialization-eligible
// CHECK-SAME: (§9.8)
