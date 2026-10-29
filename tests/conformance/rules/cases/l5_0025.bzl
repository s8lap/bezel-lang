//! case: l5_0025
//! doc: stdlib
//! section: 5
//! title: Sum, Average, Any and All resolve on IList and IReadOnlyList receivers
//! rule: The section 5 operators are extension methods on IEnumerable<T>; IReadOnlyList<T> and IList<T> extend IEnumerable<T> (stdlib 4.1), and neither interface declares a member of the called name, so the extension is reached. a is [5, 3, 8, 1]; Average over int elements returns double.
//! cites: 4.1, 5.1
//! expect: run
//! output: 17
//!   4.25
//!   True
//!   True
//!   True
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    Console.WriteLine(l.Sum());
    Console.WriteLine(r.Average());
    Console.WriteLine(l.Any());
    Console.WriteLine(r.Any(lambda(x) => x > 7));
    Console.WriteLine(r.All(lambda(x) => x > 0));
  end Main;
end Program;
