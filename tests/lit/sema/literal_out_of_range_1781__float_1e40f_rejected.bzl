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

// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// #7069: the front end's ICE marker is printed BEFORE any user-facing
// diagnostic, and a negative directive scans only the region between the
// surrounding matches — below the positive match this needle could never
// fire, so it is written above it, where it can.
// CHECK-NOT: internal compiler error
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: floating-point literal is out of range for `float`
// CHECK-SAME: (§4.3)
