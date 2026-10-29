//! case: l5_0027
//! doc: stdlib
//! section: 5
//! title: predicate and selector operator forms resolve on list interfaces
//! rule: The section 5 operators are extension methods on IEnumerable<T>; IReadOnlyList<T> and IList<T> extend IEnumerable<T> (stdlib 4.1), and neither interface declares a member of the called name, so the extension is reached. a is [5, 3, 8, 1]; Average with an int selector returns double.
//! cites: 4.1, 5
//! expect: run
//! output: 1
//!   8
//!   8
//!   3
//!   24
//!   12.75
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    Console.WriteLine(l.Last(lambda(x) => x < 5));
    Console.WriteLine(r.Single(lambda(x) => x = 8));
    Console.WriteLine(l.ElementAt(2));
    Console.WriteLine(l.Min(lambda(x) => x * 3));
    Console.WriteLine(r.Max(lambda(x) => x * 3));
    Console.WriteLine(l.Average(lambda(x) => x * 3));
  end Main;
end Program;
