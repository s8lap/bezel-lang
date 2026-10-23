// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §4.2/§4.3, v0.190 (#1781): the same range-check bounds `m`-suffixed
// `decimal` literals against `decimal`'s range (§4.3 → §4.4). `1e40m` exceeds
// `decimal.MaxValue` (79228162514264337593543950335) and once parsed silently
// to 0. Range-checked at formation via the runtime's decimal parser.
module M;
class C
  public static method Main(): int
    var x: decimal := 1e40m;
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: decimal literal is out of range for `decimal`
// CHECK-SAME: (§4.3)
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error
