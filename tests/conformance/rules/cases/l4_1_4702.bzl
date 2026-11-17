//! case: l4_1_4702
//! doc: stdlib
//! section: 4.1
//! title: the inherited Count property is read, not called, on IList,
//!   IReadOnlyList and their nullable forms
//! rule: pure readonly property Count: int;
//! cites: 4.1
//! expect: run
//! output: 3
//!   3
//!   3
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var il: IList<int> := a;
    Console.WriteLine(il.Count);
    var n := il.Count;
    Console.WriteLine(n);
    var r: IReadOnlyList<int>? := a;
    Console.WriteLine(r?.Count ?? -1);
  end Main;
end Program;
