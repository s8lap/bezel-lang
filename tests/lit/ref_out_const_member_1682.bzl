// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail — see
// tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Sema-level §22.3 category reject, ported from
//   tests/sema/ref_out_operand_category_1682_test.cpp
//   (BEZEL_TEST ref_to_const_member_rejected_22_3).
//
// Spec §22.3 (v0.184): a `const` member is a compile-time constant, not a
// storage location, so it cannot be a `ref` argument.  This is a DIFFERENT
// §22.3 sub-category from the owned-local reject (ref_out_owned_local_1682.bzl)
// — FileCheck pins the specific "`const` member" wording, demonstrating
// content matching the manifest's coarse pass/fail column cannot express.
module M;
class C
  public const K: int := 5;
  public static method Bump(ref x: int) x := x + 1; end Bump;
  public static method Main(): int
    C.Bump(ref C.K);
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a `ref` argument may not be a `const` member
// CHECK-SAME: (§22.3)
