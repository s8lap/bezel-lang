//! case: l5_1_1103
//! doc: stdlib
//! section: 5.1
//! title: Min and Max on a lazy Where or Select chain over records are typed as the element, not as int
//! rule: `Min` and `Max` without a selector are generic over comparable types ... public method
//!   MinOrNull(): T?;
//! cites: 14.2
//! expect: run
//! output: 10 20
//!   30
//! issue: 8161
module Conf;

public record R(v: int, id: int): IComparable<R>
  public method CompareTo(other: R): int => v - other.v;
end R;

public class Program
  public static method Main()
    var a: Array<R> := [new R(1, 10), new R(3, 20), new R(2, 30)];
    Console.WriteLine($"{a.Where(lambda(x: R) => true).Min().id} {a.Where(lambda(x: R) => true).Max().id}");
    Console.WriteLine($"{a.Select(lambda(x: R) => x).Where(lambda(x: R) => x.id > 15).Min().id}");
  end Main;
end Program;
