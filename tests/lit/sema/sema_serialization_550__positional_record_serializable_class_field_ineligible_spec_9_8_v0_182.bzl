// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, positional_record_serializable_class_field_ineligible_spec_9_8_v0_182).
//
// §9.8 v0.182: "A record cannot hold a class-typed component in its serialized
// set at all ... a record's reference-typed fields are always borrows, and a
// borrow has no position in an owning spanning tree — there is no owner for an
// inline write to reconstruct against, and the synthesized `Deserialize` could
// not construct the record without binding a freshly-produced component to a
// borrowing field, which §9.7's no-temporary-source rule rejects." This must
// hold even when the class field's OWN type is itself fully
// serialization-eligible (declares `ISerializable`/`IDeserializable` with only
// primitive components) — the record-vs-class distinction, not the target
// class's own eligibility, is what disqualifies it.

module M; class C: ISerializable public property V: int; public constructor(v: int) V := v; end constructor; end C; record R(X: int, Obj: C): ISerializable;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R` declares `ISerializable`/`IDeserializable` but is not serialization-eligible
// CHECK-SAME: (§9.8)
