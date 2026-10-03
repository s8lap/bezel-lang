// Fix #5331 (spec §7.4, v0.313), pin row: a `not`-composed test in a `while`
// CONDITION narrows the body — the two halves of the fix compose.
//
// §7.4 "How narrowing is established" names both ingredients: a null test
// establishes in "a `while` condition", and "`not` swaps the fact sets". With
// both in place, `while not (a = null) do …` runs its body exactly when
// `a <> null`, so `a.Length` in the body is admissible. Before the fix the
// WhileStmt arm established nothing and `collectNarrowings` had no `not` arm,
// so the read was rejected twice over.
//
// Row (b) composes the swap with an `and` (§7.4: "in `A and B`, `B` (and the
// true branch) is checked under `A`'s true-facts"): the true branch of the `and`
// sees BOTH operands' true-facts, so `a` is non-null (via the `not`-swap) while
// `b` is null (via `b = null`, whose fact is its FALSE side, and so does not
// narrow `b` on this path). The row reads `a.Length` only, which is the read the
// composition licenses.
//
// Sema-level only (compile, no run).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

class C
  public static method G(): string? return "ab"; end G;

  // Row (a): the `not`-swap alone, in a `while` condition.
  public static method NotSwap(a: string?): int
    while not (a = null) do
      Console.WriteLine(a.Length);
    end;
    return 0;
  end NotSwap;

  // Row (b): the `not`-swap composed with an `and`-conjunct.
  public static method NotSwapAnd(a: string?, b: string?): int
    while not (a = null) and (b = null) do
      Console.WriteLine(a.Length);
    end;
    return 0;
  end NotSwapAnd;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
