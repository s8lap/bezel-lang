// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #4587 (§15.3 / §28.7, spec v0.306): the `var y := x;` twin of
// `owning_choice_moved_from_read_4587__assign_spelling_rejected.bzl`. §15.3's
// first Rules bullet, "Local-to-local assignment", is stated for THIS exact
// spelling: "`var y := x;`: copies for an ordinary value type; moves for a
// move-only value type. After the move, `x` is in moved-from state ... and
// may not be read until reassigned."
//
// `Bundle` is owning-classified (§35.4): its `Ready` case carries an `owned`
// class payload, so it is a move-only VALUE type per §15.3. `var c := b;` is
// therefore a MOVE of `b`, and the later `case b of ...` reads the
// moved-from `b` — rejected, exactly as the plain-`:=` spelling is.
//
// Before the fix: `bezelc` exited 0 with zero diagnostics on this shape too
// (found while fixing #4566, the owning-choice local-to-local double-free).

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
    var c := b;
    case b of
      Ready(_) => do end,
      Empty => do end
    end;
    // Pinned exactly, matching the plain-`:=` sibling test's discipline: see
    // that file's comment for why `[[@LINE-N]]` and an exact column (not a
    // `{{[0-9]+}}` wildcard) are both required here.
    // CHECK: owning_choice_moved_from_read_4587__var_decl_spelling_rejected.bzl:[[@LINE-7]]:10: owned value in `b` was already moved out at this point (§28.3)
    return 0;
  end Main;
end C;
