// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Ported from tests/sema/private_setter_747_test.cpp
//   (BEZEL_TEST private_setter_747, write_from_other_class_rejected).
//
// §13.6: writing a `private set` property from another class is rejected.

module M; class Counter public _v: int := 0; public property Value: int get => _v; private set => do _v := value; end; end; end Counter; class Program public static method Main(): int using var c := new Counter(); c.Value := 99; return 0; end Main; end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: cannot assign to property `Value` — its setter is not accessible here
// CHECK-SAME: (§13.6)
