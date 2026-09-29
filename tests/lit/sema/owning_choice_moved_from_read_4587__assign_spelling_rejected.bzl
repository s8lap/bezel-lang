// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #4587 (§15.3 / §28.7, spec v0.306): a moved-from owning-choice LOCAL
// is still readable. §15.3's opening paragraph: "Assignment transfers the
// value from source to destination, leaving the source in moved-from state.
// Reading from a moved-from location is a compile error." Its first Rules
// bullet, "Local-to-local assignment", names this exact shape: `y := x;`
// "moves for a move-only value type. After the move, `x` is in moved-from
// state ... and may not be read until reassigned."
//
// `Bundle` is owning-classified (§35.4): its `Ready` case carries an `owned`
// class payload, so it is a move-only VALUE type per §15.3, exactly like the
// owning inline array `[N owned]T`. `c := b;` is therefore a MOVE of `b`, not
// a copy, and the later `case b of ...` reads the moved-from `b` — rejected.
//
// Before the fix (found while fixing #4566, the owning-choice local-to-local
// double-free): the ownership dataflow tracked no move at all for this
// shape, and `bezelc` exited 0 with zero diagnostics. #4566 closed the
// memory-safety half (a codegen runtime zero-store on the moved-from slot);
// this closes the compile-time half by teaching the SAME dataflow that
// already rejects the sibling move-only-array shape
// (`noteMoveOnlyArrayWholeMoveRead`, #1899) about the owning-choice kind too.

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
    case b of
      Ready(_) => do end,
      Empty => do end
    end;
    // The whole point is the diagnostic's exact text AND location, so both
    // are pinned. `[[@LINE-11]]` is relative to this CHECK, so editing the
    // header comment above cannot silently re-point it; column 10 is `b`,
    // counted from the start of the `case b of` line. A `{{[0-9]+}}` column
    // wildcard (the idiom several sibling tests use) would match ANY
    // location — including the pre-fix silent-accept's total absence of a
    // diagnostic turning into a match on an unrelated line — and pin nothing.
    // CHECK: owning_choice_moved_from_read_4587__assign_spelling_rejected.bzl:[[@LINE-11]]:10: owned value in `b` was already moved out at this point (§28.3)
    return 0;
  end Main;
end C;
