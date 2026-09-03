// Rule-3 control for §22.15 rule 6's operator arm (v0.286): "All built-in
// operators on primitives, enums, and `string`" resolve NO MethodSymbol
// (findBinaryOperator returns null for a primitive receiver), so
// `checkOperatorOverloadPurity`'s `dynamic_cast<MethodSymbol*>(opSym)` guard
// must fall through and leave every one of these alone. A fix that
// classified operators by TOKEN instead of by resolved overload would wrongly
// reject this row.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
public class Program
  public static pure method Prims(a: int, b: int): int
    var t := a - b;
    var neg := -a;
    return t + neg;
  end Prims;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
