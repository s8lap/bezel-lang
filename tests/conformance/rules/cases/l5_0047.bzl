//! case: l5_0047
//! doc: stdlib
//! section: 5
//! title: Min with a selector and Average with a selector over int list interfaces
//! rule: Min<U>(selector) returns U and Average(selector: (T) -> int) returns double (stdlib 5); a is [5, 3, 8, 1].
//! cites: 5
//! expect: run
//! output: 3
//!   12.75
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
    Console.WriteLine(l.Min(lambda(x) => x * 3));
    Console.WriteLine(r.Average(lambda(x) => x * 3));
  end Main;
end Program;
