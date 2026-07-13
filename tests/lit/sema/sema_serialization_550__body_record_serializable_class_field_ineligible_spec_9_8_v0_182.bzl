// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, body_record_serializable_class_field_ineligible_spec_9_8_v0_182).
//
// Same rule, body-record form (auto-property component instead of a
// positional field) — §9.8's body-record membership rule shares the same
// component-eligibility check as the positional-record rule above.

module M; class C: ISerializable public property V: int; public constructor(v: int) V := v; end constructor; end C; record R: ISerializable public property Obj: C; public constructor(obj: C) Obj := obj; end constructor; end R;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R` declares `ISerializable`/`IDeserializable` but is not serialization-eligible
// CHECK-SAME: (§9.8)
