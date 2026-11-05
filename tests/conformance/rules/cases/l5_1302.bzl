//! case: l5_1302
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull on Zip stops at the shorter input; on SelectMany it spans all inner sequences
//! rule: null when index is out of range
//! cites: 15.1
//! expect: run
//! output: 21
//!   -1
//!   b
//!   none
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<int>();
    xs.Add(1);
    xs.Add(2);
    xs.Add(3);
    using var ys := new Array<string>();
    ys.Add("a");
    ys.Add("b");
    var z := xs.Zip(ys, lambda(a, b) => a * 10 + b.Length).ElementAtOrNull(1);
    Console.WriteLine(z ?? -1);
    var z2 := xs.Zip(ys, lambda(a, b) => a * 10 + b.Length).ElementAtOrNull(2);
    Console.WriteLine(z2 ?? -1);
    var s := xs.SelectMany(lambda(x) => ys).ElementAtOrNull(3);
    Console.WriteLine(s ?? "none");
    var s2 := xs.SelectMany(lambda(x) => ys).ElementAtOrNull(6);
    Console.WriteLine(s2 ?? "none");
  end Main;
end Program;
