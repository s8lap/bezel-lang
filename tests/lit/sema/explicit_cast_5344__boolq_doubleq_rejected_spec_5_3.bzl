// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #5344 (§5.3, v0.314): `bool?(double?)` should also be rejected.

module M;
class C
  public static method Main(): int
    var d: double? := 1.0;
    var b: bool? := bool?(d);
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: conversion between a numeric type and `bool` is not permitted (§5.1/§5.2, §5.3 nullable operand)
