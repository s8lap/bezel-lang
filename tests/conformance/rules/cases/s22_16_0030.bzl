//! case: s22_16_0030
//! doc: spec
//! section: 22.16
//! title: a generic method with an ensures clause returns a generic choice by expression and block body
//! rule: Contract clauses sit between a signature and its body (§22.16, v0.340) and add nothing to the value a body returns; "For a **generic** choice the type arguments are written at the construction (`Result<int, string>.Ok(value)`)" (§35.3, v0.340); a generic member is evaluated at each instantiation after substituting the type arguments (§3.6 rule 2, §14.1), so the payload constructed in the generic body is the payload matched by the caller. Contract clauses: the contract clause `ensures(n >= 0)` (§22.16, v0.340) holds for `n = 1`, and adds nothing to the result: `Wrap<int>(5, 1)` (expression body) and `WrapB<int>(6, 1)` (block body) return `Some(5)` and `Some(6)`.
//! cites: 35.3, 14.1
//! expect: run
//! output: 5
//!   6
module Conf;

public choice Opt<T>
  Some(v: T),
  No;
end Opt;

public class Api
  public static method Wrap<T>(x: T, n: int): Opt<T>
    ensures(n >= 0)
  => Opt<T>.Some(x);

  public static method WrapB<T>(x: T, n: int): Opt<T>
    ensures(n >= 0)
    var o := Opt<T>.Some(x);
    return o;
  end WrapB;
end Api;

public class Program
  public static method Main()
    Console.WriteLine((case Api.Wrap<int>(5, 1) of Some(var v) => $"{v}", No => "no"));
    Console.WriteLine((case Api.WrapB<int>(6, 1) of Some(var v) => $"{v}", No => "no"));
  end Main;
end Program;
