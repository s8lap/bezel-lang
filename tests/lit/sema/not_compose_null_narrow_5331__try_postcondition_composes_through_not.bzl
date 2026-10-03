// Fix #5331 (spec §7.4 / §22.4, v0.313), pin row: a `when true` nullability
// postcondition composes through `not` and an early exit, like any other fact.
//
// §7.4 "How narrowing is established", last bullet: a call declaring a
// `when true`/`when false` postcondition on an `out` recipient (§22.4) narrows
// "in any region where the call's `bool` result is statically known to equal
// the stated constant, the recipient is narrowed non-null, composing through
// boolean operators and early exits like any other fact". The `not`-swap is one
// of those boolean operators.
//
// In `if not p.Get(out var y) then return 1; end;` the branch is taken when
// `Get` returned FALSE; the surviving path is the one where it returned TRUE,
// which is exactly the `when true` constant, so `y` is non-null after the `if`.
// Before the fix `collectNarrowings` reached no `UnaryExpr`, so no fact was
// recorded and `y + 1` was wrongly rejected.
//
// The builtin shape-(a) Try members carry the same postcondition normatively
// (stdlib §1, fixed for the positive direction by #5312). This row uses the
// DECLARED form so the pin is a pure sema fact and does not depend on the
// builtin `not`-lowering path; the builtin positive-direction companion is
// builtin_shape_a_when_true_5312__owningset_trypop_predeclared_accepted.bzl.
//
// Sema-level only (compile, no run).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class P
  public method Get(out x: int? when true): bool
    x := 1;
    return true;
  end Get;
end P;

class C
  // Row (a): the issue #5331 repro for a declared `when true` call.
  public static method Main(): int
    var p := new P();
    if not p.Get(out var y) then return 1; end;
    return y + 1;
  end Main;

  // Row (b): the same composition as a PRE-DECLARED recipient rather than an
  // inline `out var` — the fact is keyed on the recipient symbol either way.
  public static method PreDeclared(p: P): int
    var y: int? := null;
    if not p.Get(out y) then return 1; end;
    return y + 1;
  end PreDeclared;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
