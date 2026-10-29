//! case: l2_5_0011
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan comparison of the extremes, interpolation holes, and unary negation of MinValue
//! rule: `CompareTo` is tick order; an interpolation hole with or without `:c`
//!   produces the constant-form rendering; the unary `-` on a `TimeSpan` raises
//!   `OverflowException` on `MinValue`.
//! cites: 2.5, 2.18
//! expect: run
//! output: True
//!   -10675199.02:48:05.4775808 10675199.02:48:05.4775807 -10675199.02:48:05.4775808
//!   overflow
module Conf;

public class Program
  public static method Main()
    var z: TimeSpan := TimeSpan.MinValue;
    var m: TimeSpan := TimeSpan.MaxValue;
    Console.WriteLine(z.CompareTo(m) < 0);
    Console.WriteLine($"{z} {m} {z:c}");
    try
      var y := -z;
      Console.WriteLine($"no exception {y}");
    catch OverflowException =>
      Console.WriteLine("overflow");
    end;
  end Main;
end Program;
