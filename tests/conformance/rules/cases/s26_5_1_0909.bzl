//! case: s26_5_1_0909
//! doc: spec
//! section: 26.5.1
//! title: extensions on Array<int> beat the same-named LINQ operators on IEnumerable<int> for an
//!   Array<int>
//! rule: An extension on E1 is **more specific** than one on E2 when E1 is a subtype of E2 and E2 is not
//!   a subtype of E1.
//! cites: 26.5.1
//! expect: run
//! output: 101
//!   102
//!   103
//!   104
//!   105
//!   106
//!   107
//!   108
//!   109
//!   110
//!   111
//!   112
//!   113
//!   114
//!   115
//!   116
module Conf;

extension Array<int>
  public method Where(p: (int) -> bool): int => 101;
  public method Select(f: (int) -> int): int => 102;
  public method SelectMany(f: (int) -> Array<int>): int => 103;
  public method OrderBy(k: (int) -> int): int => 104;
  public method OrderByDescending(k: (int) -> int): int => 105;
  public method Distinct(): int => 106;
  public method Union(o: Array<int>): int => 107;
  public method Intersect(o: Array<int>): int => 108;
  public method Except(o: Array<int>): int => 109;
  public method Take(n: int): int => 110;
  public method TakeWhile(p: (int) -> bool): int => 111;
  public method Skip(n: int): int => 112;
  public method SkipWhile(p: (int) -> bool): int => 113;
  public method Concat(o: Array<int>): int => 114;
  public method ToSet(): int => 115;
  public method ToDictionary(k: (int) -> int): int => 116;
end;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2, 3];
    var b := new Array<int>();
    b.Add(4);
    Console.WriteLine(a.Where(lambda(x) => x > 1));
    Console.WriteLine(a.Select(lambda(x) => x));
    Console.WriteLine(a.SelectMany(lambda(x) => b));
    Console.WriteLine(a.OrderBy(lambda(x) => x));
    Console.WriteLine(a.OrderByDescending(lambda(x) => x));
    Console.WriteLine(a.Distinct());
    Console.WriteLine(a.Union(b));
    Console.WriteLine(a.Intersect(b));
    Console.WriteLine(a.Except(b));
    Console.WriteLine(a.Take(1));
    Console.WriteLine(a.TakeWhile(lambda(x) => x > 1));
    Console.WriteLine(a.Skip(1));
    Console.WriteLine(a.SkipWhile(lambda(x) => x > 1));
    Console.WriteLine(a.Concat(b));
    Console.WriteLine(a.ToSet());
    Console.WriteLine(a.ToDictionary(lambda(x) => x));
  end Main;
end Program;
