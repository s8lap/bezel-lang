// Ported from tests/module/pure_operator_rule6_3610_test.cpp
//   (BEZEL_TEST pure_operator_rule6_3610, local_pure_binary_operator_accepted).
//
// NO-FALSE-REJECT control for pure_operator_rule6_3610_local_binary_reject.bzl:
// the character-identical twin with `pure ` on the operator declaration must
// still compile — a fix that rejected every operator regardless of purity
// would fail this row while passing the reject row.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
public class Vec
  public X: int;
  public constructor(x: int)
    X := x;
  end constructor;
  public static pure method operator -(a: Vec, b: Vec): int => a.X - b.X;
end Vec;
public class Program
  public static pure method P(a: Vec, b: Vec): int
    return a - b;
  end P;
end Program;

// NODIAG-NOT: error
// NODIAG-NOT: §
