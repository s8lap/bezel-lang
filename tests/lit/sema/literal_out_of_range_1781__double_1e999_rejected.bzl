// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// spec §4.2/§4.3, v0.190 (#1781): a floating-point literal denotes the nearest
// representable value of its type; a literal whose magnitude exceeds the type's
// largest finite value is a compile error AT FORMATION — it must NOT silently
// round to ±Infinity (§6.3's overflow-to-Infinity governs arithmetic, not
// literals). `1e999` overflows `double` and once parsed silently to +Infinity.
//
// FileCheck matches the rule wording + the §4.3 citation, and CHECK-NOT guards
// against an internal-compiler-error escape rather than a clean user diagnostic.
module M;
class C
  public static method Main(): int
    var x: double := 1e999;
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: floating-point literal is out of range for `double`
// CHECK-SAME: (§4.3)
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error
