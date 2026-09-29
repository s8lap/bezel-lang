// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, record_explicit_ctor_no_match_rejected_spec_9_8_v0182).
//
// §9.8 v0.182: a body record whose sole explicit constructor's parameter set
// does NOT bijection-match the serialized components (an extra/mismatched
// name) is a compile error naming the candidate — "no constructor has
// parameter names matching the serialized component set."

module M; record Bad: ISerializable, IDeserializable public property N: int; public constructor(m: int) N := m + 1; end constructor; end Bad;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Bad` declares `IDeserializable`: no explicit constructor has parameter names matching the serialized component set {N} — candidates: constructor(m)
// CHECK-SAME: (§9.8 step 2, v0.309: constructor selection is by name, case-insensitive, order-insensitive; the synthesized primary is the fallback candidate when the record declares positional fields)
