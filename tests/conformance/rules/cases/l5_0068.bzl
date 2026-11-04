//! case: l5_0068
//! doc: stdlib
//! section: 5
//! title: Where, Select and Count, Sum, Average, Any run over an IList-typed receiver
//! rule: public method Where(predicate: (T) -> bool): IEnumerable<T>;
//! cites: 4.1, 5
//! expect: run
//! output: 3
//!   34
//!   4.25
//!   True
//! issue: 7625
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [5, 3, 8, 1];
    var l: IList<int> := a;
    var n: int := l.Where(lambda(x: int): bool => x > 1).Count();
    var s: int := l.Select(lambda(x: int): int => x * 2).Sum();
    var avg: double := l.Select(lambda(x: int): int => x).Average();
    var any: bool := l.Where(lambda(x: int): bool => x > 7).Any();
    Console.WriteLine(n);
    Console.WriteLine(s);
    Console.WriteLine(avg);
    Console.WriteLine(any);
  end Main;
end Program;
