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

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: floating-point literal is out of range for `double`
// CHECK-SAME: (§4.3)
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error
