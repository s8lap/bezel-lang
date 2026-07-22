// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.8 choice serialization (v0.200, #2046): the borrow-bearing classification is
// never serialization-eligible (§35.4). A choice with a plain (unmarked) class-
// typed payload is borrow-bearing; declaring the conformance is a compile error
// at the conformance declaration.

module M;

class Widget(id: int) public property Id: int => id; end Widget;

choice Holder: ISerializable, IDeserializable Wraps(w: Widget), Empty; end Holder;

class P public static method Main(): int return 0; end Main; end P;

// CHECK: choice `Holder` declares `ISerializable`/`IDeserializable` but is not serialization-eligible (§9.8)
// CHECK-SAME: borrow-bearing (§35.4)
