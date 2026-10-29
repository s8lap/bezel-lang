//! case: l5_0048
//! doc: stdlib
//! section: 5
//! title: ToArray of a list interface and of a string list interface produce independent arrays
//! rule: ToArray() returns a fresh Array<T> (stdlib 5): writing the copy leaves the source unchanged, and a string list copies to an Array<string> with the same count.
//! cites: 5
//! expect: run
//! output: 5
//!   99
//!   cde
//!   2
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
    var arr := l.ToArray();
    arr[0] := 99;
    Console.WriteLine(a[0]);
    Console.WriteLine(arr[0]);
    var sarr := sr.ToArray();
    Console.WriteLine(sarr[1]);
    Console.WriteLine(sarr.Count);
  end Main;
end Program;
