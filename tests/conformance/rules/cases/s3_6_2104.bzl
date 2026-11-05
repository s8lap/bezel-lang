//! case: s3_6_2104
//! doc: spec
//! section: 3.6
//! title: a T-typed Sum() result cannot initialise an int local in a
//!   generic body
//! rule: A generic body type-checks under the weakest assumption its
//!   constraints establish
//! cites: 5.1, 14.2
//! expect: reject
//! diagnostic: constraints
//! issue: 8266
//! notes: Red on base: the pre-fix compiler typed the no-selector Sum()
//!   over a type-parameter element as the concrete int and accepted this
//!   program, the staging tip rejects it.
module Conf;

public class Program
  public static method G<T>(a: Array<T>): int where T: INumber<T>
    var s := a.Sum();
    var n: int := s;
    return n;
  end G;

  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    Console.WriteLine(G(a));
  end Main;
end Program;
