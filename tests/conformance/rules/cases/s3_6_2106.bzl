//! case: s3_6_2106
//! doc: spec
//! section: 3.6
//! title: a Set<T> receiver's Sum() is T, not int, in a generic body
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
  public static method G<T>(a: Set<T>): int where T: INumber<T> => a.Sum();

  public static method Main()
    var a := new Set<int>();
    a.Add(5);
    Console.WriteLine(G(a));
  end Main;
end Program;
