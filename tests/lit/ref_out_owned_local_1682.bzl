// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail — see
// tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// ★ FLAGSHIP — why this whole lit track exists.
//
// Ported from tests/sema/ref_out_operand_category_1682_test.cpp
//   (BEZEL_TEST ref_to_plain_owned_local_rejected_22_3).
//
// Spec §22.3 (v0.184): a `ref`/`out` argument may not name OWNED state — an
// owned local, an `owned` parameter/field, or an owning-collection slot — because
// a ref/out write is an assignment through an alias, invisible to the
// drop-on-overwrite (§28.7) / consume-site-freeze (§28.3) machinery owned state
// relies on.  Passing an owned local `b` by `ref` is therefore a compile error.
//
// THE STRUCTURAL POINT (issue #1682): this rejection is emitted by the
// OWNERSHIP pass, which runs AFTER sema.  The in-process gtest for it had to
// reach for `parseAndCheckOwnership` because the project's ordinary sema-only
// harness (`runJit`) bails at the post-sema `hasErrors()` gate and never carries
// the ownership pass's diagnostics — so a test written against the sema-only
// harness would see NO error here and silently pass while the reject went
// unobserved.  That exact "wrong-harness → reject invisible" mistake cost a gate
// cycle this week.  lit cannot make that mistake: it invokes the REAL `bezelc`,
// which ALWAYS runs the full pipeline (lex → parse → sema → ownership →
// codegen), so the §22.3 ownership diagnostic is always in the observed output.
module M;
class Box public V: int := 0; end Box;
class C
  public static method Swallow(ref x: Box) x := new Box(); end Swallow;
  public static method Main(): int
    var b := new Box();
    C.Swallow(ref b);
    return 0;
  end Main;
end C;

// The ownership pass names the rule (§22.3) and the offending category
// ("owned local `b`"), not a generic "some error".  bezelc emits diagnostics as
// `file:line:col: message` with no "error:" severity word, so we anchor on the
// `:line:col:` shape and the rule text.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: a `ref` argument may not be owned local `b`
// CHECK-SAME: (§22.3
