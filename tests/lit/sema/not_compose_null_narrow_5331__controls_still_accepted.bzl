// Fix #5331 (spec §7.4 / §22.4, v0.313) — the CONTROL rows. Every shape here
// compiled BEFORE the fix and must still compile after it: the change is
// additive (a `while`-body `collectNarrowings` call and a `not` arm), so a
// regression on any of these rows means the fix widened narrowing where §7.4
// does not license it.
//
// §7.4's establishing positions that #5331 touches but does not change:
//   * "in an `if`/`elif` condition … each condition narrowing its own branches"
//     — rows (a) and (b);
//   * "an early exit: after `if x = null then return; end;` … the surviving
//     path is narrowed" — row (c), the UN-negated guard, which is the mirror of
//     the new `not` arm;
//   * §22.4 "the controlled branch of `if TryX(…, out var c) then`" — row (d),
//     the POSITIVE-direction Try narrowing fixed for the builtins by #5312 and
//     held here so the flipped recurse cannot disturb it.
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
  public static method G(): string? return "ab"; end G;

  // Row (a): the plain `if`-then narrowing, both branches.
  public static method IfThenElse(a: string?): int
    if a <> null then
      return a.Length;
    else
      return 0;
    end;
  end IfThenElse;

  // Row (b): the `if` narrows its own branches without a `while` or a `not`.
  public static method IfElif(a: string?, b: string?): int
    if a <> null then
      return a.Length;
    elif b <> null then
      return b.Length;
    end;
    return 0;
  end IfElif;

  // Row (c): the un-negated early-exit guard — the direct mirror of the new
  // `not` arm, and the shape the issue quotes alongside it.
  public static method EarlyExitGuard(a: string?): int
    if a = null then return 1; end;
    return a.Length;
  end EarlyExitGuard;

  // Row (d): positive-direction `when true` Try narrowing in an `if` (both an
  // inline `out var` and a pre-declared recipient).
  public static method PositiveTry(p: P): int
    if p.Get(out var x) then
      return x + 1;
    else
      return 0;
    end;
  end PositiveTry;

  public static method PositiveTryPreDeclared(p: P): int
    var y: int? := null;
    if p.Get(out y) then
      return y + 1;
    end;
    return 0;
  end PositiveTryPreDeclared;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
