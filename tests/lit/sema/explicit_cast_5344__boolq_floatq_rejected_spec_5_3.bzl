// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #5344 (§5.3, v0.314): `bool?(float?)` should also be rejected.
// Per §5.3's general rule: `T?(x)` is admitted exactly when the
// corresponding non-nullable conversion `T(v)` is admitted.

module M;
class C
  public static method Main(): int
    var f: float? := 1.0f;
    var b: bool? := bool?(f);
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: conversion between a numeric type and `bool` is not permitted (§5.1/§5.2, §5.3 nullable operand)
