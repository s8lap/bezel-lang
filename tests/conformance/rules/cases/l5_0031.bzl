//! case: l5_0031
//! doc: stdlib
//! section: 5
//! title: the null-conditional call of ToArray on a nullable list interface yields null for a null receiver
//! rule: ?. short-circuits on a null receiver (spec 7.5); on a non-null IList<int>? the section 5 ToArray() extension is reached, so the count is 3 for [1, 2, 3] and 0 through ?? for the null receiver.
//! cites: 7.5, 5
//! expect: run
//! output: 3
//!   0
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    var b: Array<int> := [1, 2, 3];
    var x: IList<int>? := b;
    var n: IList<int>? := null;
    var t := x?.ToArray();
    var u := n?.ToArray();
    Console.WriteLine(t?.Count ?? 0);
    Console.WriteLine(u?.Count ?? 0);
  end Main;
end Program;
