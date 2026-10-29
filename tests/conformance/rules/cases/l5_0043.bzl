//! case: l5_0043
//! doc: stdlib
//! section: 5
//! title: First, Last and ElementAt over string list interfaces return strings
//! rule: First, Last and ElementAt return T (stdlib 5), here string, so string members apply to the result.
//! cites: 5
//! expect: run
//! output: abx
//!   2
//!   5
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
    Console.WriteLine(sl.First() + "x");
    Console.WriteLine(sr.First().Length);
    Console.WriteLine(sl.Last().Length + sr.ElementAt(0).Length);
  end Main;
end Program;
