// Fix #5331 (spec §7.4, v0.313) — the AFTER-LOOP edge. This is the reject half
// of the while-body change: establishing the condition's fact for the body must
// NOT let it survive the loop.
//
// §7.4 "How narrowing is established" makes the `while` condition an
// establishing position, but a `while` body may execute ZERO times. On that path
// the condition was never evaluated true, so nothing was established about `a`
// at the after-loop point. §7.4 "How narrowing is invalidated" and issue #2118
// already pin this shape for a narrowing established INSIDE the body
// (sema_null_narrow__loop_body_narrowing_does_not_leak_2118.bzl); #5331 widens
// the establishing position to the CONDITION, so the same rule has to keep
// holding for the fact the condition itself introduces.
//
// The fix therefore takes the `whileEntryNull` / `whileEntryNarrowings` /
// `whileEntryField` snapshots BEFORE calling `collectNarrowings` on the
// condition, and the `intersectNarrowingSetsAfterLoop` join restores exactly
// that entry state afterwards. Taking the snapshots after the call would leak
// the fact and wrongly accept the read below.
//
// Sema-level only (compile, no run).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

class C
  public static method G(): string? return "ab"; end G;

  // Zero iterations -> `a` was never tested, so it is still `string?` here.
  public static method AfterLoop(s: string?): int
    while s <> null do
      Console.WriteLine(s.Length);
    end;
    return s.Length;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first
    // CHECK-SAME: (§7.4)
  end AfterLoop;

  // A `break` is a possible exit from the body, and it exits WITHOUT the
  // condition having been re-tested on that path — the edge snapshot (#2152)
  // folds it into the same join, so the after-loop read is rejected too.
  public static method AfterBreak(s: string?): int
    while s <> null do
      break;
    end;
    return s.Length;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first
    // CHECK-SAME: (§7.4)
  end AfterBreak;
end C;
