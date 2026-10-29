//! case: l5_0059
//! doc: stdlib
//! section: 5
//! title: The IList Count property is accepted inside a lambda body and as a Math.Max argument
//! rule: Only an instance member blocks an extension (spec 26.5 rule 1); l.Count is a property read of 3, so f(1) is 4 and Math.Max(3, 2) is 3.
//! cites: 4.1
//! expect: run
//! output: 4
//!   3
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var l: IList<int> := a;
    var f := lambda(n: int) => n + l.Count;
    Console.WriteLine(f(1));
    Console.WriteLine(Math.Max(l.Count, 2));
  end Main;
end Program;
