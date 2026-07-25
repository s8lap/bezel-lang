// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail — see
// tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Spec §22.3 (v0.226) / #1756 regression guard: a `ref`/`out` argument
// aliases the caller's storage location directly (§22.3 "ref and out
// argument locations"), so its type must EXACTLY match the parameter's
// referent type — no §5.1 implicit conversion is admitted (the same
// "aliases the caller's location, so no conversion is admitted" principle
// §14.5 states for the generic-inference case, here in the concrete,
// non-generic position).
//
// Pre-fix, checkArgsAgainstParams unconditionally skipped EVERY modifier-
// bearing argument, so this concrete ref/out slot was never type-checked:
// a `ref` argument of type `long` against a `ref x: int` parameter compiled
// clean and aliased mismatched-width storage through the parameter (the
// issue's repro). It must now be REJECTED with the §22.3 diagnostic.
module M;
class C
  public static method Inc(ref x: int) x := x + 1; end Inc;
  public static method Main(): int
    var n: long := 5;
    C.Inc(ref n);
    return 0;
  end Main;
end C;

// The §22.3 diagnostic names the offending modifier (`ref`), the parameter
// (`x`), and the no-conversion principle (§5.1). bezelc emits diagnostics as
// `file:line:col: message` with no "error:" severity word, so we anchor on the
// `:line:col:` shape and the rule text.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `ref` argument for parameter `x` to {{.*}} does not match the parameter's referent type
// CHECK-SAME: a `ref`/`out` argument aliases the caller's location, so the type must match exactly and no implicit conversion is admitted
// CHECK-SAME: (§22.3)
