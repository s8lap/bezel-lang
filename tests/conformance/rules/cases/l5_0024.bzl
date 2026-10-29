//! case: l5_0024
//! doc: stdlib
//! section: 5
//! title: element accessors First, Last, ElementAt, Min and Max resolve on IList and IReadOnlyList receivers
//! rule: The section 5 operators are extension methods on IEnumerable<T>; IReadOnlyList<T> and IList<T> extend IEnumerable<T> (stdlib 4.1), and neither interface declares a member of the called name, so the extension is reached. a is [5, 3, 8, 1].
//! cites: 4.1, 5.1, 26.5
//! expect: run
//! output: 8
//!   1
//!   1
//!   5
//!   3
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    Console.WriteLine(l.Max());
    Console.WriteLine(r.Min());
    Console.WriteLine(l.Last());
    Console.WriteLine(r.First());
    Console.WriteLine(r.ElementAt(1));
  end Main;
end Program;
