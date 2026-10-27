// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=CHECK --allow-empty %s
//
// Issue #4587 (§15.3 / §28.7, spec v0.306) accept control: a read AFTER
// reassignment must still be admitted. §15.3's first Rules bullet states the
// read is rejected only "until reassigned" — once `b` is given a fresh value
// via a second local-to-local move (`c := b;`) followed by a plain
// reassignment (`b := Bundle.Ready(...)`), the moved-from state is cleared
// and the subsequent `case b of ...` reads a live value again. This guards
// against the fix over-rejecting: a broken implementation that left `b`
// permanently poisoned once moved-from (never re-armed by a reassignment)
// would fail this file with the SAME diagnostic the sibling reject tests
// pin, on the `case b of` line below.

module M;
class Resource
  public constructor() end constructor;
  destructor end destructor;
end Resource;
choice Bundle
  Ready(owned r: Resource),
  Empty;
end Bundle;
class C
  public static method Main(): int
    var b := Bundle.Ready(new Resource());
    var c := Bundle.Ready(new Resource());
    c := b;
    b := Bundle.Ready(new Resource());
    case b of
      Ready(_) => do end,
      Empty => do end
    end;
    return 0;
  end Main;
end C;

// fix-forward #3575 (§35.4, v0.334): `Bundle.Ready(...)` is a CASE
// CONSTRUCTION, and §35.3 makes an owning construction an "owned move-only
// value"; §35.4 then requires the K3.14 must-use warning on a local it binds
// that is never discharged. `b` and `c` are read only, through a BORROWING
// `case` (§35.5 — an unmarked read of a move-only scrutinee stays a borrowing
// match), so neither transfers the obligation and both warn. That warning is
// the CORRECT §35.4 behaviour and it is not a rejection: the control this file
// exists for — "a read AFTER reassignment is admitted" — is unchanged.
//
// The blanket "no diagnostic may cite a section" directive this replaces
// forbade the two must-use warnings above, which are required to cite §28.3.
// Pinning the exact warnings restores strictly MORE coverage than it drops:
// a NOT-for-`error` line brackets each warning, so a rejection anywhere in the
// output still fails the file, and the `already moved out` guards keep the
// sibling #4587 reject rows' needle out of an accept row that shares their
// declarations.

// CHECK-NOT: error
// #7065: the same needle, written ABOVE the positive match. A negative
// directive scans only the region between the surrounding matches, so the
// copy below the match alone let this wording through when it came FIRST.
// CHECK-NOT: already moved out
// CHECK: warning: owned value in `b` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop)
// CHECK-NOT: error
// CHECK-NOT: already moved out
// CHECK: warning: owned value in `c` is implicitly disposed at the end of its scope (§28.3 must-use / auto-drop)
// CHECK-NOT: error
// CHECK-NOT: already moved out
