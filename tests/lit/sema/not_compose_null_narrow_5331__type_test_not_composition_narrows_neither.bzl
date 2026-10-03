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
// classification fact, so the `UnaryExpr` arm declines a `TypeTestExpr` operand.
// The two rows below are the shapes that naive recursion would wrongly accept.
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
  public constructor(x: int) X := x; end constructor;
end D;

class C
  // Row (a): `not (o is not D)` — the truth value equals `o is D`, which does
  // narrow; but §7.4 says `not`-composition narrows neither branch, so the
  // classification fact must NOT be established here. The read is rejected.
  public static method NotIsNot(o: B?): int
    if not (o is not D) then
      return 0;
    end;
    var q := o as D;
    return q.X;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.X` on a nullable value requires narrowing it to non-null first
    // CHECK-SAME: (§7.4)
  end NotIsNot;

  // Row (b): the same limit with `as` — `o as D` on a possibly-null `o` is not
  // a proof of anything, and the `not` contributed no fact.
  public static method NotIs(o: B?): int
    if not (o is D) then
      return 0;
    end;
    var q := o as D;
    return q.X;
    // CHECK: :[[@LINE-1]]:{{[0-9]+}}: member access `.X` on a nullable value requires narrowing it to non-null first
    // CHECK-SAME: (§7.4)
  end NotIs;
end C;
