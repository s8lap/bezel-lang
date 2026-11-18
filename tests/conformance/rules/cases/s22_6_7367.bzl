//! case: s22_6_7367
//! doc: spec
//! section: 22.6
//! title: a one-parameter lambda is inapplicable to a zero-parameter function
//!   slot of another constructor
//! rule: a candidate whose function-typed slot has a different number of
//!   parameter slots from the argument's
//! cites: 27.3
//! expect: run
//! stdout: A
//! notes: Pins the #6642 fix: the lambda has one parameter, so the `() ->
//!   int` constructor is inapplicable at its position (a count difference is
//!   inapplicability, never an error), and the `(int) -> ()` constructor is
//!   selected; the expression body `x + 1` is a result-discarding procedure
//!   lambda there.
module Conf;

public class K
  public constructor(f: (int) -> ()) Console.WriteLine("A"); end;
  public constructor(f: () -> int) Console.WriteLine("B"); end;
end;
public static class Program
  public static method Main(): int
    var k := new K(lambda(x: int) => x + 1);
    return 0;
  end;
end;
