// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// §6b mandatory exclusion-diff control: a BUILTIN collection index
// (`Array<T>`) read inside a `pure` body resolves no user `PropertySymbol`
// at the ExprTypecheck_Exprs.cpp typing site (the `setExprSymbol` call
// there is gated on a NAMED receiver type with a matching user-declared
// indexer candidate), so the new §22.15 rule 6 arm added by fix #3721 is a
// no-op for this shape. This row proves the fix does not false-reject the
// overwhelmingly common builtin-index-in-pure-body pattern.

module App;
public class Program
  public static pure method P(a: Array<int>): int
    return a[0];
  end P;
  public static method Main(): int => 0;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
