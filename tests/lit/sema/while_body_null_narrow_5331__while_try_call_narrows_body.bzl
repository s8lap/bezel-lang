// Fix #5331 (spec §7.4 / §22.4, v0.313), pin row 2 of the while-body arm: a
// `when true` nullability POSTCONDITION in a `while` condition narrows its `out`
// recipient in the body, exactly as a null test does.
//
// §7.4 "How narrowing is established", last bullet: a call declaring a
// `when true`/`when false` postcondition on an `out` recipient (§22.4) narrows
// "in any region where the call's `bool` result is statically known to equal
// the stated constant, the recipient is narrowed non-null". A `while`
// condition is such a region on its true side. §22.4 names the composing
// positions: "the controlled branch of `if TryX(…, out var c) then`, an
// `and`-conjunct, an early-exit guard" — a `while` body is the loop analogue of
// the controlled branch.
//
// Note the DIRECTION. This row pins `while p.Get(out var x) do …`, not a
// `not`-negated call. `not p.Get(out var x)` in a `while` condition means the
// body runs when `Get` returned FALSE, so the recipient is NOT narrowed there —
// writing `x + 1` in that body is a compile error, and the flipped-recurse arm
// in `collectNarrowings` correctly produces no fact for it. The negative
// direction is pinned by not_compose_null_narrow_5331__not_try_call_body_not_narrowed.bzl.
//
// Sema-level only (compile, no run).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class P
  // §22.4: the `when true` postcondition is the declared form of the fact
  // (the builtin shape-(a) Try members carry it normatively via stdlib §1).
  public method Get(out x: int? when true): bool
    x := 1;
    return true;
  end Get;
end P;

class C
  public static method Main(): int
    var p := new P();
    while p.Get(out var x) do
      return x + 1;
    end;
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
