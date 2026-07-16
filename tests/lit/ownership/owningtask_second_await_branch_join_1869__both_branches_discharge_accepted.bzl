// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// §30.5 (Pattern B) / Appendix C.6 — issue #1869 (SB01-C2). POSITIVE control:
// the branch-join await-spend reject must NOT over-fire when the task IS
// discharged on EVERY arm. The maybe-consumed marker is set only when one arm
// spent the task and another arm left it Owned; when both arms discharge it,
// the join has no state disagreement and nothing is rejected.
//
//   AwaitOrDispose : await on one arm, explicit Dispose() on the other — both
//                    discharge; no use after the join.
//   AwaitBoth      : await on both arms — both spend; no use after the join.
//
// These guard against a naive fix that marked any `Owned ⊓ Disposed` join
// maybe-consumed (which would break the #601 idempotent one-branch-Dispose
// pattern and reject both methods here). The await-spend upgrade is scoped to
// the genuine one-arm-spent / other-arm-Owned disagreement.

module M;

class Cell public constructor() end constructor; end Cell;

class C
  public static async method MakeAsync(): OwningTask<Cell>
    return new Cell();
  end MakeAsync;

  public static async method AwaitOrDispose(flag: bool): Task
    var t := C.MakeAsync();
    if flag then
      using var g := await t;        // spent on this arm
    else
      t.Dispose();                   // discharged on this arm
    end;
    // t discharged on BOTH arms — no maybe-consumed, nothing to reject.
  end AwaitOrDispose;

  public static async method AwaitBoth(flag: bool): Task
    var t := C.MakeAsync();
    if flag then
      using var g1 := await t;       // spent on this arm
    else
      using var g2 := await t;       // spent on this arm too
    end;
    // t spent on BOTH arms — no maybe-consumed, nothing to reject.
  end AwaitBoth;
end C;

// NODIAG-NOT: maybe-consumed
// NODIAG-NOT: already awaited
