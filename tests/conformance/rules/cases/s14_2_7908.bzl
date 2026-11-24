//! case: s14_2_7908
//! doc: spec
//! section: 14.2
//! title: Sum forwarded through a type parameter U with no INumber bound is rejected
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per
//!   the rule above.
//! cites: 14.2, 5
//! expect: reject
//! diagnostic: inumber
module Conf;
import System;

public class Program
  static method F<T, U>(a: Array<T>, f: (T) -> U): U => a.Sum(f);
  public static method Main()
    var a := new Array<double>();
    a.Add(1.5);
    a.Add(2.25);
    Console.WriteLine(F(a, lambda(x) => x * 2.0));
  end Main;
end Program;
