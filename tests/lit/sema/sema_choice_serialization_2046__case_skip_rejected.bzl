// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.8 choice serialization (v0.200, #2046): "The `serialized` clause on a CASE
// admits the rename form only (a case is not omittable — the enum-constant rule)."
// `serialized skip` on a case is a compile error.

module M;

choice Bad: ISerializable, IDeserializable A serialized skip, B; end Bad;

class P public static method Main(): int return 0; end Main; end P;

// CHECK: choice `Bad`: the `serialized` clause on case `A` admits the rename form only
// CHECK-SAME: a case is not omittable (§9.8
