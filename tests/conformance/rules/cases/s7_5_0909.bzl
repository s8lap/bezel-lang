//! case: s7_5_0909
//! doc: spec
//! section: 7.5
//! title: a null receiver skips a ?.Select(...) chain entirely; a present one runs the lambda
//! rule: no member is read, no call is performed, and the argument and index expressions of the
//!   skipped suffixes are not evaluated either
//! cites: 27.4
//! expect: run
//! output: -1
//!   hit
//!   hit
//!   2
//! issue: 8166
module Conf;

public class Program
  public static method Hit(x: int): int
    Console.WriteLine("hit");
    return x;
  end Hit;

  public static method Main()
    using var arr := new Array<int>();
    arr.Add(5);
    arr.Add(6);
    var a: Array<int>? := arr;
    var n: Array<int>? := null;
    var z := n?.Select(lambda(x) => Hit(x))?.ToArray();
    Console.WriteLine(z?.Count ?? -1);
    var y := a?.Select(lambda(x) => Hit(x))?.ToArray();
    Console.WriteLine(y!.Count);
  end Main;
end Program;
