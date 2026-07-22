// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.8 choice serialization (v0.200, #2046): "The `$case` field name joins the
// `$`-reserved set of stdlib §2.15's JSON mapping; a payload rename beginning
// with `$` is a compile error." The same reservation applies to a CASE rename.

module M;

choice Bad: ISerializable, IDeserializable A serialized "$weird", B; end Bad;

class P public static method Main(): int return 0; end Main; end P;

// CHECK: choice `Bad`: `serialized` rename `$weird` for case `A` begins with `$`
// CHECK-SAME: reserved for the JSON $-set
