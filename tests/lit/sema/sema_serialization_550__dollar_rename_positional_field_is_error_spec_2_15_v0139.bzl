// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/serialization_synthesis_550_sema_test.cpp
//   (BEZEL_TEST sema_serialization_550, dollar_rename_positional_field_is_error_spec_2_15_v0139).
//
// §2.15 v0.139: a positional record field with a `serialized "$xxx"` rename is
// a compile error — `$`-initial wire names are reserved for JSON graph-identity.
// Syntax: `Name: Type serialized "wireName"` (SerializedClause follows the type).

module M; record R(X: int serialized "$foo"): ISerializable, IDeserializable;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `R`: `serialized` rename `$foo` for field `X` begins with `$`, which is reserved for JSON graph-identity tokens
// CHECK-SAME: (§2.15 v0.139)
