// RUN: %bezelc -c -o %t.o %s
//
// Helper dependency artifact for the #3610 cross-module operator-invocation
// pair (pure_operator_rule6_3610_xmod_reject.bzl /
// pure_operator_rule6_3610_xmod_accept.bzl). `Sub` is not `pure`-declared;
// `Add` is — so an importer's `pure` body may call `Add` but not `Sub`,
// under §22.15 rule 6 / §2.8.2 the artifact interface. Its own RUN line is
// an unremarkable library accept (§2.4).
module OpLib3610;

public class Vec
  public X: int;
  public constructor(x: int)
    X := x;
  end constructor;
  public static pure method operator +(a: Vec, b: Vec): int => a.X + b.X;
  public static method operator -(a: Vec, b: Vec): int => a.X - b.X;
end Vec;
