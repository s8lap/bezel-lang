// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §9.7 / stdlib §4.15 (v0.226, issue #1251): the field-INITIALIZER position of
// the same §9.7 instance-field storage rule the assignment-site lit pins —
// `MethodOwnership::checkFieldInitializer` carried the identical
// `isBorrowBearingRecord` gap, so `f: ArraySpan<int> := …` in a class body was
// accepted. §9.7: "A borrow-bearing record value may not be stored in an
// **instance field of a class** … the store is **rejected statically**."

module M; class Src public static readonly a: Array<int> := new Array<int>(); end Src; class Box public f: ArraySpan<int> := Src.a.AsSpan(); end Box; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a span may not be stored in the instance field `f`
// CHECK-SAME: (§9.7; store the backing store instead
