// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #5344 (§5.3, v0.314): `bool?(decimal?)` should be rejected.
// Per §5.3's general rule: `T?(x)` is admitted exactly when the
// corresponding non-nullable conversion `T(v)` is admitted. Since
// `bool(decimal)` is not admitted (§5.2: truthiness-shaped, partial),
// `bool?(decimal?)` is not admitted either.

module M;
class C
  public static method Main(): int
    var d: decimal? := 1.0m;
    var b: bool? := bool?(d);
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: conversion between a numeric type and `bool` is not permitted (§5.1/§5.2, §5.3 nullable operand)
