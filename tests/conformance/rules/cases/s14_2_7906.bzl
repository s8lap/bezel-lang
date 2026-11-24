//! case: s14_2_7906
//! doc: spec
//! section: 14.2
//! title: Sum over a selector returning int? is rejected (a bound admits only non-nullable
//!   types)
//! rule: a class or interface bound admits only non-nullable substitutions, as `where T:
//!   class` does
//! cites: 14.2, 5
//! expect: reject
//! diagnostic: nullable
module Conf;
import System;

public class Program
  static method Opt(x: int): int? => x;
  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    a.Add(2);
    Console.WriteLine(a.Sum(lambda(x) => Opt(x)));
  end Main;
end Program;
