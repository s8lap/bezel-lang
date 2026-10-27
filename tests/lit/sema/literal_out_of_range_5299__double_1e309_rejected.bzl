// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §4.3, v0.313 (#5299): the underflow fix must not loosen the overflow
// side. `1e309` rounds (to nearest) to +Infinity, so it is still a compile error.
module M;
class C
  public static method Main(): int
    var x := 1e309;
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
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: floating-point literal is out of range for `double`
// CHECK-SAME: (§4.3)
