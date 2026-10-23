// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §4.2/§4.3, v0.190 (#1781): a float literal whose magnitude exceeds
// `float`'s largest finite value (~3.4e38) is a compile error at formation.
// `1e40f` is finite as a `double` but overflows `float`, and once materialized
// silently to +Infinity (codegen parses via strtod then truncates to f32).
module M;
class C
  public static method Main(): int
    var x: float := 1e40f;
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: floating-point literal is out of range for `float`
// CHECK-SAME: (§4.3)
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error
