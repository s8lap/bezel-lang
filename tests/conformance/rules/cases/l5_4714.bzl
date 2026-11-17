//! case: l5_4714
//! doc: stdlib
//! section: 5
//! title: a null-conditional Count() on a nullable IEnumerable reaches the
//!   LINQ operator
//! rule: public method Count(): int;
//! cites: 5, 1
//! expect: run
//! stdout: 3
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var e: IEnumerable<int>? := a;
    Console.WriteLine(e?.Count() ?? -1);
  end Main;
end Program;
