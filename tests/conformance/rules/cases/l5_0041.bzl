//! case: l5_0041
//! doc: stdlib
//! section: 5
//! title: Sum and Max over long list interfaces keep the element type long
//! rule: Sum() is the INumber<T> generic returning the element's own type (stdlib 5.1) and Max() returns T; over long elements 5000000000 + 3 + 8 does not wrap, and the result passes as a long argument.
//! cites: 5.1, 5
//! expect: run
//! output: 5000000012
//!   5000000000
//!   5000000011
//!   5000000011
//!   15000000011
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
    Console.WriteLine(ll.Sum() + 1);
    Console.WriteLine(lr.Max());
    var s: long := ll.Sum();
    Console.WriteLine(s);
    Console.WriteLine(Id(ll.Sum()));
    Console.WriteLine(Pick(ll));
  end Main;
end Program;
