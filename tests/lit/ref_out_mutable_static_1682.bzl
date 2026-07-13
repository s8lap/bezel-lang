// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail — see
// tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Sema-level §22.3 category reject, ported from
//   tests/sema/ref_out_operand_category_1682_test.cpp
//   (BEZEL_TEST ref_to_mutable_static_rejected_22_3).
//
// Spec §22.3 + §22.12 rule 2 (v0.184): a mutable static field's store
// semantics are defined for direct assignment only, so it is not a valid
// `ref` operand.  This diagnostic cites TWO spec sections in one message
// (§22.3 and §22.12); FileCheck's `CHECK-SAME` matches both on the same line,
// which a single-column golden expectation cannot represent.
module M;
class C
  public static Count: int := 0;
  public static method Bump(ref x: int) x := x + 1; end Bump;
  public static method Main(): int
    C.Bump(ref C.Count);
    return C.Count;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a `ref` argument may not be the mutable static field `Count`
// CHECK-SAME: §22.12 rule 2
// CHECK-SAME: (§22.3)
