// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, positional_record_non_serializable_record_field_ineligible_spec_9_8).
//
// §9.8: another ineligible case — record whose nested record is NOT itself
// ISerializable/IDeserializable but is used as a field type.
// A record field type is eligible only if that record is itself
// serialization-eligible (§9.8: "another serialization-eligible record").

module M; record Inner(V: int); record Outer(A: Inner, B: int): ISerializable;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Outer` declares `ISerializable`/`IDeserializable` but is not serialization-eligible
// CHECK-SAME: (§9.8)
