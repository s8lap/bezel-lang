// Fix #5331 (spec §7.4 / §22.4, v0.313) — the DIRECTION guard on the flipped
// recurse. This is the reject track that keeps the `not` arm from recording a
// fact on the wrong side.
//
// The `not` arm recurses with `forTrueBranch` flipped, so `not p.Get(out var y)`
// asks for `p.Get`'s FALSE side. For a `when true` postcondition that side
// establishes nothing: the call returned false, so the recipient was NOT
// written with a value and stays `int?`. Reading it in the body — the region
// the `not`-condition controls — is therefore a compile error, and the arm must
// not widen the fact to make it pass.
//
// If the flip were dropped (recursing with the un-flipped flag, or recording
// unconditionally) this row would be wrongly ACCEPTED, so it is the negative
// counterpart of the accept row in
// not_compose_null_narrow_5331__try_postcondition_composes_through_not.bzl.
//
// Sema-level only (compile, no run).
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;

class P
  public method Get(out x: int? when true): bool
    x := 1;
    return true;
  end Get;
end P;

class C
  // Row (a): the `not`-negated call in a `while` condition. The body runs when
  // `Get` returned FALSE, so `x` is not narrowed there.
  public static method NotTryInWhile(p: P): int
    while not p.Get(out var x) do
      return x + 1;
      // CHECK: :[[@LINE-1]]:{{[0-9]+}}: operator `+` is not defined on a nullable value type
      // CHECK-SAME: (§7.4)
    end;
    return 0;
  end NotTryInWhile;

  // Row (b): the same negation in an `if` — the branch is entered when `Get`
  // returned FALSE, so the recipient is still `int?` inside it. Only the path
  // that SURVIVES the early exit is narrowed.
  public static method NotTryInThenBranch(p: P): int
    if not p.Get(out var y) then
      return y + 1;
      // CHECK: :[[@LINE-1]]:{{[0-9]+}}: operator `+` is not defined on a nullable value type
      // CHECK-SAME: (§7.4)
    end;
    return 0;
  end NotTryInThenBranch;
end C;
