// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #2162 (§6.6/§10, spec v0.226) — the arithmetic twin of the bitwise
// case: `x += Flags.B` on `x: Flags` was silently admitted for the same
// reason (a Named TARGET fell through both §6.6 family branches). §6.2's
// mixed-type promotion covers primitive numeric operands only, §10 forbids
// the implicit enum↔integer conversion (§5.4 requires the explicit cast), so
// `x + Flags.B` has no result R unless `Flags` declares `operator +` (§25.2);
// §6.6: "In every other case the compound form is a compile error." The
// binary form `Flags.A + Flags.B` was already rejected (`operator + is not
// defined for type Flags`).

module M;
enum Flags A, B, C; end Flags;
class C public static method Main(): int
  var x: Flags := Flags.A;
  x += Flags.B;
  return 0;
end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `+=` has no valid result on enum type `Flags`
// CHECK-SAME: (§6.6/§10.4)
