// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, positional_record_shared_field_ineligible_spec_9_8_v0_182).
//
// §9.8 v0.182: "The same holds for `Shared<T>` ... a record declares no
// owning spanning tree against which an identity participant ... could
// resolve." A record field of type `Shared<C>` (C itself serialization-
// eligible) is not serialization-eligible.

module M; class C: ISerializable public property V: int; public constructor(v: int) V := v; end constructor; end C; record R(X: int, Obj: Shared<C>): ISerializable;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R` declares `ISerializable`/`IDeserializable` but is not serialization-eligible
// CHECK-SAME: (§9.8)
