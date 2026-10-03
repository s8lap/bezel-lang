// Fix #5331 (spec §7.4, v0.313), pin row: `not` swaps the fact sets, so a
// `not`-negated null test composed with an EARLY EXIT narrows the surviving
// path.
//
// §7.4 "How narrowing is established", second bullet: "boolean composition of
// tests: … `not` swaps the fact sets — the standard three-outcome flow
// analysis". Third bullet: "an early exit: after `if x = null then return;
// end;` (or `throw`, `break`, `continue` in that branch), the surviving path is
// narrowed".
//
// The two compose. In `if not (b <> null) then return; end;` the branch is
// taken exactly when `b IS null`, so the path that survives the `if` has
// `b <> null`. Before the fix `collectNarrowings` had no `UnaryExpr` arm, so
// nothing was recorded and the trailing `b.Length` was wrongly rejected — the
// issue #5331 repro.
//
// The `not`-composed form is what this row pins; the un-negated guard
// (`if b = null then return; end;`) already worked and is held as a control in
// not_compose_null_narrow_5331__controls_still_accepted.bzl.
//
// Sema-level only (compile, no run).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class C
  public static method G(): string? return "ab"; end G;

  // Row (a): the issue #5331 repro — `not` over a `<> null` test + early exit.
  public static method Main(): int
    var b := C.G();
    if not (b <> null) then return 1; end;
    Console.WriteLine(b.Length);
    return 0;
  end Main;

  // Row (b): the same swap written over the `=` spelling. `if not (b = null)`
  // is taken exactly when `b <> null`, so the THEN branch is the narrowed one —
  // the `not` turned the test's false-fact ("b is null", what `b = null`
  // establishes) into the then-branch's true-fact ("b is not null"). The read
  // is inside the branch the negation controls, with no early exit involved,
  // which isolates the swap itself from the exit composition of row (a).
  public static method NegatedEqualsSpelling(b: string?): int
    if not (b = null) then
      return b.Length;
    end;
    return 0;
  end NegatedEqualsSpelling;

  // Row (c): the parser desugars `is not null` to `<> null` (Parser_Exprs.cpp
  // §17.4), so §7.4's `not`-swap applies to the `is not null` spelling too —
  // it is the same null fact under a different surface syntax.
  public static method IsNotNullSpelling(b: string?): int
    if not (b is not null) then return 1; end;
    return b.Length;
  end IsNotNullSpelling;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
