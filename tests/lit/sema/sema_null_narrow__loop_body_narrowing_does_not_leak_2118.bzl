// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter: the compile is EXPECTED to fail, and
// wrapping it in `not` lets its diagnostics flow through lit's pipe to
// FileCheck — see tests/lit/lit.cfg.py and README.md.)
//
// Spec §7.4 "How narrowing is established" (v0.220, issue #2118): narrowing is
// flow-sensitive — "at each program point the compiler tracks, per local,
// whether its value is known non-null." A loop body may execute ZERO times, so
// a narrowing ESTABLISHED inside a `while`/`for` body does NOT hold on the
// zero-iteration path and must NOT survive past the loop. Before the fix, the
// while/for sema handlers walked the body without joining the narrowing state,
// so an in-body `:=` / `??=` narrowing leaked to the after-loop point and a
// possibly-null local was wrongly accepted for a bare member access.
//
// This reject track pins the leak edges (which a behavior/accept test cannot
// express — the fix REJECTS these). The surviving-pre-loop-narrowing and
// in-body-use ACCEPT controls are in the companion accept track
// (tests/lit/sema/sema_null_narrow__loop_pre_narrowing_survives_2118.bzl and,
// for in-body use, tests/codegen/null_narrow_coalesce_assign_2104_test.cpp).
module M;
class C
  // Edge 1 — `while` + `:=`: a non-nullable value assigned inside the body
  // narrows `s`, but the body may not run, so `s` may still be null after.
  public static method WhileAssignLeak(cond: bool, seed: string): int
    var s: string? := null;
    while cond do
      s := seed;
    end;
    return s.Length;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first (§7.4)
  end WhileAssignLeak;

  // Edge 2 — `while` + `??=`: a non-nullable RHS narrows `s` inside the body
  // (§7.7), but on the zero-iteration path `s` is untouched and still null.
  public static method WhileCoalesceLeak(cond: bool): int
    var s: string? := null;
    while cond do
      s ??= "hi";
    end;
    return s.Length;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first (§7.4)
  end WhileCoalesceLeak;

  // Edge 3 — `for` + `:=`: an empty range runs the body zero times, so the
  // in-body narrowing must not leak past the loop either.
  public static method ForAssignLeak(seed: string): int
    var s: string? := null;
    for var i in 0..2 do
      s := seed;
    end;
    return s.Length;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first (§7.4)
  end ForAssignLeak;

  // Edge 4 (invalidation direction still fires): a narrowing established BEFORE
  // the loop is invalidated by an in-body reassignment from a nullable value
  // (§7.4 "How narrowing is invalidated"), so the after-loop access is rejected.
  public static method BodyInvalidates(s: string?, other: string?, cond: bool): int
    if s <> null then
      while cond do
        s := other;
      end;
      return s.Length;
      // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first (§7.4)
    end;
    return -1;
  end BodyInvalidates;
end C;
