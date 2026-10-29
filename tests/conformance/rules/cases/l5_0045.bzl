//! case: l5_0045
//! doc: stdlib
//! section: 5
//! title: operator results interpolate, combine and condition like ordinary expressions
//! rule: The section 5 operators return ordinary typed values (stdlib 5): First() on IReadOnlyList<int> is an int, Sum() on IList<int> is an int; a is [5, 3, 8, 1].
//! cites: 5
//! expect: run
//! output: 17 5
//!   6
//!   ok
//!   big
//!   3
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
    Console.WriteLine($"{l.Sum()} {r.First()}");
    var x := l.First();
    var y := r.Last();
    Console.WriteLine(x + y);
    if l.Any() and r.Contains(8) then
      Console.WriteLine("ok");
    end;
    if l.Sum() > 10 then
      Console.WriteLine("big");
    end;
    var k := 0;
    while k < l.Sum() / 5 do
      k := k + 1;
    end;
    Console.WriteLine(k);
  end Main;
end Program;
