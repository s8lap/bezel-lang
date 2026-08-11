// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #3044 (§9.8 / §17.11, v0.242): §9.8 — "`serialized skip` … the synthesized
// `Deserialize` does not read it and supplies the component's `default(T)`
// (§17.11). A `serialized skip` component whose type has no well-defined default
// is a compile error at the conformance declaration."
//
// §17.11's not-well-defined set gained the enum row at v0.242, so a `serialized
// skip` component of a zero-less enum is now such a component. Without the
// check it reconstructs at 0 — a value equal to no declared constant, exactly
// what §5.4's invariant forbids, arriving through synthesis rather than a cast.

module M;
import System.Serialization;

enum Level Low := 1, Mid := 2; end Level;

record Doc(Title: string, Rank: Level serialized skip): ISerializable, IDeserializable;

class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: {{.*}}`serialized skip` on field `Rank`
// CHECK-SAME: declares no constant of underlying value 0
