//! case: l5_0044
//! doc: stdlib
//! section: 5
//! title: selector Sum and Max and predicate First and Last resolve on list interfaces
//! rule: The section 5 operators are extension methods on IEnumerable<T>; IReadOnlyList<T> and IList<T> extend IEnumerable<T> (stdlib 4.1), and neither interface declares a member of the called name, so the extension is reached. a is [5, 3, 8, 1].
//! cites: 5
//! expect: run
//! output: 34
//!   16
//!   8
//!   1
module Conf;

public class Program
  public static method Id(x: long): long => x;
  public static method Pick(l: IList<long>): long => l.Sum() + l.Max() + l.First();

  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var r: IReadOnlyList<int> := a;
    var la: Array<long> := [5000000000L, 3L, 8L];
    var ll: IList<long> := la;
    var lr: IReadOnlyList<long> := la;
    var da: Array<double> := [1.5, 2.5];
    var dl: IList<double> := da;
    var dr: IReadOnlyList<double> := da;
    var sa: Array<string> := ["ab", "cde"];
    var sl: IList<string> := sa;
    var sr: IReadOnlyList<string> := sa;
    Console.WriteLine(l.Sum(lambda(x) => x * 2));
    Console.WriteLine(r.Max(lambda(x) => x * 2));
    Console.WriteLine(l.First(lambda(x) => x > 5));
    Console.WriteLine(r.Last(lambda(x) => x < 5));
  end Main;
end Program;
