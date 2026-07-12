// Clean-compile ACCEPT control, ported from
//   tests/sema/ref_out_operand_category_1682_test.cpp
//   (BEZEL_TEST ref_to_plain_local_accepted_22_3).
//
// Spec §22.3 (v0.184): a mutable local variable is the canonical ADMITTED
// `ref` operand category.  `Inc(ref n)` where `n` is a plain `var` local must
// compile clean — no §22.3 diagnostic, no error of any kind.
//
// Two RUN lines assert the accept two ways:
//  1. `bezelc -c` must EXIT ZERO — lit fails the test if the command returns
//     non-zero, so a successful compile IS the pass.  Output goes to `%t.o`
//     (build-tree scratch), never the source tree.
//  2. The diagnostic stream must be empty of errors — FileCheck with
//     `--allow-empty` (there is no output) confirms neither "error" nor a §
//     section-citation reject appears.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
module M;
class C
  public static method Inc(ref x: int) x := x + 1; end Inc;
  public static method Main(): int
    var n := 5;
    Inc(ref n);
    return n;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
