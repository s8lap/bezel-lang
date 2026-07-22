// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.8 choice serialization (v0.200, #2046): "A choice is serialization-eligible
// when it is value-like (§35.4) ... the borrow-bearing and owning classifications
// are never eligible — the same Deserialize-has-no-owner argument as for
// class-typed record fields. An ineligible conformance declaration is the compile
// error above, naming the first offending payload."
//
// An OWNING choice (an `owned` class payload) that declares the conformance is a
// compile error at the conformance declaration.

module M;

class Conn(id: int) public property Id: int => id; end Conn;

choice Res: ISerializable, IDeserializable Ok(owned c: Conn), Err(code: int); end Res;

class P public static method Main(): int return 0; end Main; end P;

// CHECK: choice `Res` declares `ISerializable`/`IDeserializable` but is not serialization-eligible (§9.8)
// CHECK-SAME: owning (§35.4)
