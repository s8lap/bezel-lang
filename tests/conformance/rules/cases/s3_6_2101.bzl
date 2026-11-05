//! case: s3_6_2101
//! doc: spec
//! section: 3.6
//! title: a generic body cannot return the no-selector Sum() over T as int
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
  public static method G<T>(a: Array<T>): int where T: INumber<T> => a.Sum();

  public static method Main()
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    Console.WriteLine(G(a));
  end Main;
end Program;
