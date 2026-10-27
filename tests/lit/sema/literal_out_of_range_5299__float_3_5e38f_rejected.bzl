// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §4.3, v0.313 (#5299): `3.5e38f` exceeds float.MaxValue plus half an ulp
// (3.4028236e38), so it still rounds to +Infinity: a compile error, while the
// underflowing `1e-50f` is accepted.
module M;
class C
  public static method Main(): int
    var x: float := 3.5e38f;
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
