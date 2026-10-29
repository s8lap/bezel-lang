//! case: l5_0042
//! doc: stdlib
//! section: 5
//! title: Average, Sum and Max over double list interfaces keep the element type double
//! rule: Average over double returns double and Sum() returns the element's own type (stdlib 5, 5.1); for [1.5, 2.5] the average is 2, the sum 4 and twice the maximum 5.
//! cites: 5.1, 5
//! expect: run
//! output: 2
//!   4
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
    var d: double := dr.Average();
    Console.WriteLine(d);
    Console.WriteLine(dl.Sum());
    Console.WriteLine(dl.Max() * 2.0);
  end Main;
end Program;
