//! case: l5_1301
//! doc: stdlib
//! section: 5
//! title: ElementAtOrNull indexes OrderBy, Distinct, Concat, Select, Where, Take and Skip results
//! rule: null when index is out of range
//! cites: 15.1
//! expect: run
//! output: 1
//!   9
//!   -1
//!   1
//!   -1
//!   20
//!   -1
//!   -77
//!   6
//!   -1
//!   9
//!   9
module Conf;

public class Program
  public static method Main()
    using var xs := new Array<int>();
    xs.Add(5);
    xs.Add(3);
    xs.Add(3);
    xs.Add(9);
    xs.Add(1);
    using var ys := new Array<int>();
    ys.Add(10);
    ys.Add(20);
    Console.WriteLine(xs.OrderBy(lambda(x) => x).ElementAtOrNull(0) ?? -1);
    Console.WriteLine(xs.OrderBy(lambda(x) => x).ThenBy(lambda(x) => x).ElementAtOrNull(4) ?? -1);
    Console.WriteLine(xs.OrderBy(lambda(x) => x).ElementAtOrNull(5) ?? -1);
    Console.WriteLine(xs.Distinct().ElementAtOrNull(3) ?? -1);
    Console.WriteLine(xs.Distinct().ElementAtOrNull(4) ?? -1);
    Console.WriteLine(xs.Concat(ys).ElementAtOrNull(6) ?? -1);
    Console.WriteLine(xs.Concat(ys).ElementAtOrNull(7) ?? -1);
    Console.WriteLine(xs.Concat(ys).ElementAtOrNull(-1) ?? -77);
    Console.WriteLine(xs.Select(lambda(x) => x * 2).Where(lambda(x) => x > 5).ElementAtOrNull(1) ?? -1);
    Console.WriteLine(xs.Take(2).ElementAtOrNull(2) ?? -1);
    Console.WriteLine(xs.Skip(1).Take(3).ElementAtOrNull(2) ?? -1);
    var v := xs.Where(lambda(x) => x > 3).ElementAtOrNull(1);
    if v <> null then
      Console.WriteLine(v!);
    else
      Console.WriteLine("null");
    end if;
  end Main;
end Program;
