// Fix #5331 (spec §7.4, v0.313) — the LIMIT on the `not`-swap. This is the
// reject track that keeps the new `UnaryExpr` arm from establishing a fact
// §7.4 explicitly withholds from `not`-composition.
//
// §7.4 "How narrowing is established", second bullet, is precise about the
// swap's scope: "`not` swaps the fact sets — the standard three-outcome flow
// analysis … The `not`-swap applies to **null facts**; a *type-classification*
// fact is established only by the `is`/`is not` operators and typed patterns
// directly, never by `not`-composition — `not (x is T)` has the same truth
// value as `x is not T` but narrows NEITHER branch".
//
// A naive `collectNarrowings(ue->operand, !forTrueBranch)` recurse would hand
// the flipped flag to `narrowFromTypeTest` and establish exactly that
// classification fact, so the `UnaryExpr` arm declines a `TypeTestExpr` operand
// (ExprTypecheck.cpp:14164). The two rows below are the shapes that naive
// recursion would wrongly accept.
//
// PIN SHAPE (reviewer-verified by mutation). Each row reads a member of the
// controlled operand `o` directly, INSIDE the branch the `not`-condition
// selects, so the read is gated on the very fact §7.4 withholds. `narrowFromTypeTest`
// keys off the branch, not off the truth value: for `is` it narrows the TRUE
// branch, for `is not` the FALSE branch. Flipping `forTrueBranch` therefore
// lands the fact on the OPPOSITE side of each test, which is exactly the two
// mutations the guard prevents:
//
//   row (a) `not (o is not D)` — reading in the THEN branch. The un-flipped
//     `is not` narrows its FALSE branch; the flipped recurse narrows the THEN
//     branch instead, and `o` would wrongly read as a non-null `B` there.
//   row (b) `not (o is D)` — reading in the ELSE branch. The un-flipped `is`
//     narrows its TRUE branch; the flipped recurse narrows the ELSE branch
//     instead, and `o` would wrongly read as a non-null `D` (hence `Y`, the
//     `D`-only member, is the read that shows the over-narrowing).
//
// `Y` is declared on `D` and NOT on `B` so row (b)'s read is possible only under
// the wrongly-established classification fact — a read of the shared member `X`
// could not tell a `D` from a `B`.
//
// NO intermediate local is used. `var d: D := o;` and `var d := o as D;` both
// reject on their own nullability/initializer rules (a §7 "initializer for `d`
// is nullable" and a §7.4 nullable-member-access respectively) whether or not
// the guard is in place, so they pin nothing about this guard. The read must
// hang off `o` itself for the row to be mutation-sensitive.
//
// MUTATION PROOF: delete the leading `!` on ExprTypecheck.cpp:14164, rebuild
// bezelc/bezel_tests_sema, and BOTH rows are ACCEPTED — the bug returns. With
// the guard restored both REJECT with the §7.4 diagnostic below.
//
// Contrast the NULLABILITY spellings: the parser desugars `x is null` /
// `x is not null` to `x = null` / `x <> null` (Parser_Exprs.cpp §17.4), so those
// ARE plain null tests and DO swap under `not` — pinned as an accept row in
// not_compose_null_narrow_5331__early_exit_guard_narrows_survivor.bzl.
//
// Sema-level only (compile, no run).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

class B
  public X: int;
  public constructor(x: int) X := x; end constructor;
end B;

class D
  public X: int;
  public Y: int;
  public constructor(x: int) X := x; Y := x; end constructor;
end D;

class C
  // Row (a): `not (o is not D)` — the truth value equals `o is D`, which does
  // narrow; but §7.4 says `not`-composition narrows neither branch, so the
  // classification fact must NOT be established in the then branch. A naive
  // flipped recurse would narrow it there and wrongly accept `o.X`.
  public static method NotIsNot(o: B?): int
    if not (o is not D) then
      return o.X;
      // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.X` on a nullable value requires narrowing it to non-null first
      // CHECK-SAME: (§7.4)
    else
      return 0;
    end;
  end NotIsNot;

  // Row (b): `not (o is D)` — the §7.4 type-classification case itself. The
  // `not` contributes no fact, so the `else` block must read as if the test had
  // not been evaluated. A naive flipped recurse would narrow `o` to a non-null
  // `D` there and wrongly accept the `D`-only member `Y`.
  public static method NotIs(o: B?): int
    if not (o is D) then
      return 0;
    else
      return o.Y;
      // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.Y` on a nullable value requires narrowing it to non-null first
      // CHECK-SAME: (§7.4)
    end;
  end NotIs;
end C;
