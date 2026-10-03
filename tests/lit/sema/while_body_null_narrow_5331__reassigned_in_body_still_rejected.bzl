// Fix #5331 (spec §7.4, v0.313) — INVALIDATION still fires inside the while
// body. Establishing the condition's fact must not make the body immune to
// §7.4 "How narrowing is invalidated".
//
// §7.4 "How narrowing is invalidated": "The narrowing of a variable is
// invalidated when, within the controlled block: the variable is reassigned from
// a nullable-typed expression". The controlled block for a `while` condition is
// the body, so a body-local reassignment from a nullable value drops the fact
// the condition just established, and the later member read in the SAME body is
// rejected. This is the in-body analogue of the after-loop edge pinned by
// while_body_null_narrow_5331__condition_fact_does_not_survive_loop.bzl and of
// the pre-existing #2118 body tracks.
//
// Sema-level only (compile, no run).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

class C
  // `s` is non-null on entry to each iteration (the condition proved it), but
  // the body reassigns it from a nullable value, which drops the fact.
  public static method ReassignedInBody(s: string?, other: string?): int
    while s <> null do
      Console.WriteLine(s.Length);   // OK — the condition's fact is in effect
      s := other;                    // §7.4 invalidation: reassigned from T?
      Console.WriteLine(s.Length);   // REJECTED — the fact is gone
      // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Length` on a nullable value requires narrowing it to non-null first
      // CHECK-SAME: (§7.4)
    end;
    return 0;
  end ReassignedInBody;
end C;
