//! case: s14_2_7907
//! doc: spec
//! section: 14.2
//! title: Sum over a selector returning int is admitted and folds
//! rule: `where T: IFoo` admits exactly the substitutions for which T conforms to `IFoo` per
//!   the rule above.
//! cites: 14.2, 5
//! expect: run
//! stdout: 3
module Conf;
import System;

public class Program
  static method Opt(x: int): int? => x;
  public static method Main()
    var a := new Array<int>();
    a.Add(1);
    a.Add(2);
    Console.WriteLine(a.Sum(lambda(x) => x));
  end Main;
end Program;
