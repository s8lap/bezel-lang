//! case: l5_4712
//! doc: stdlib
//! section: 5
//! title: Count() on an IEnumerable, which declares no Count member, reaches
//!   the LINQ operator
//! rule: public method Count(): int;
//! cites: 5, 1
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var e: IEnumerable<int> := a;
    Console.WriteLine(e.Count());
  end Main;
end Program;
