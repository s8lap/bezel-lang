//! case: l5_0067
//! doc: stdlib
//! section: 5
//! title: Reverse on an IList-typed receiver is the lazy operator and leaves the source unchanged
//! rule: the lazy operator applies only where the receiver's static type has no
//!   instance `Reverse`
//! cites: 4.1, 4.4, 5, 26.5
//! expect: run
//! output: 4
//!   1
//!   4
//! issue: 7625
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3, 4];
    var l: IList<int> := a;
    using var rv := l.Reverse().ToArray();
    Console.WriteLine(rv[0]);
    Console.WriteLine(a[0]);
    a.Reverse();
    Console.WriteLine(a[0]);
  end Main;
end Program;
