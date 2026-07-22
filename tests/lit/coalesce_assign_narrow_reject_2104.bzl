// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter: the compile is EXPECTED to fail, and
// wrapping it in `not` lets its diagnostics flow through lit's pipe to
// FileCheck — see tests/lit/lit.cfg.py and README.md for the full rationale.)
//
// Spec §7.4 "How narrowing is established" (v0.210, issue #2104): `x ??= expr`
// narrows `x` to non-null ONLY when `expr` is non-nullable ("on every path `x`
// then holds a non-null value", §7.7). This reject track pins the two edges of
// that rule the behavior/accept track (tests/codegen/
// null_narrow_coalesce_assign_2104_test.cpp) cannot express:
//
//   1. a NULLABLE RHS does NOT narrow — after `s ??= t` with `t: string?`, the
//      was-null-then-null path leaves `s` null, so `s` is not proved non-null;
//   2. the §7.4 invalidation edges still fire AFTER a narrowing `??=` — passing
//      the narrowed local as `ref` invalidates the narrowing exactly as it does
//      after a plain `:=`, so a subsequent member access is again rejected.
module M;
class C
  // Edge 1: nullable RHS must NOT establish narrowing.
  public static method NullableRhs(t: string?): int
    var s: string? := null;
    s ??= t;
    return s.Length;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first (§7.4)
  end NullableRhs;

  public static method Clobber(ref s: string?): ()
    s := null;
  end Clobber;

  // Edge 2: a narrowing `??=` (non-nullable RHS) narrows `s`, but passing it as
  // `ref` invalidates that narrowing per §7.4, so the later access is rejected.
  public static method RefStillInvalidates(): int
    var s: string? := null;
    s ??= "hello";
    C.Clobber(ref s);
    return s.Length;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first (§7.4)
  end RefStillInvalidates;
end C;
