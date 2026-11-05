//! case: s7_5_0908
//! doc: spec
//! section: 7.5
//! title: a LINQ lambda after ?. on a nullable Array<int> local sees int elements, int and string projections
//! rule: a suffix to the right of a `?.` is typed at the value it meets on the path where it runs
//!   — the preceding segment's own declared type, never the nullable type the `?.` gives the
//!   chain's result
//! cites: 27.4
//! expect: run
//! output: 7
//!   v
//! issue: 8166
module Conf;

public class Program
  public static method Main()
    using var arr := new Array<int>();
    arr.Add(5);
    arr.Add(6);
    var a: Array<int>? := arr;
    var r := a?.Select(lambda(x) => x + 1)?.ToArray();
    Console.WriteLine(r![1]);
    var q := a?.Select(lambda(x) => "v")?.ToArray();
    Console.WriteLine(q![0]);
  end Main;
end Program;
