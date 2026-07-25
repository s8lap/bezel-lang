// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #2162 (§6.5/§6.6/§3.5/§10, spec v0.226) — a compound bitwise
// assignment on an ENUM target was silently admitted: the §6.5 target-kind
// guard requires a Primitive target, and the §6.6 family check's two
// branches covered only primitive-target/primitive-RHS and primitive-target/
// Named-RHS, so a Named TARGET fell through both and the program compiled
// (codegen emitted an unspecified value). The binary form `Flags.A | Flags.B`
// was already correctly rejected. §10: "An enum is not implicitly convertible
// to or from its underlying integer type" (§5.4 requires the explicit cast),
// §6.5 bitwise is the eight integer primitives, and §6.6: "In every other
// case the compound form is a compile error" — so `x |= Flags.B` on `x: Flags`
// is rejected unless `Flags` declares an applicable `operator |` (§25.2).

module M;
enum Flags A, B, C; end Flags;
class C public static method Main(): int
  var x: Flags := Flags.A;
  x |= Flags.B;
  return 0;
end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: compound assignment `|=` has no valid result on enum type `Flags`
// CHECK-SAME: (§6.6/§10.4)
