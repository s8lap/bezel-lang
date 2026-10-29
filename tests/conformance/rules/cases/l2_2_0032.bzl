//! case: l2_2_0032
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs/Max/Min/Clamp/Floor/Ceiling/Truncate accept a narrowed decimal?
//! rule: stdlib §2.2 declares the decimal overloads Abs, Floor, Ceiling, Truncate and the generic Min/Max/Clamp. §7.4 (v0.350): narrowing is flow-sensitive and "a null test ... `x <> null` ... narrowing the true branch"; a narrowed nullable local has the non-nullable type, so a stdlib parameter of the underlying type accepts it. With m = -2.5m: Abs 2.5, Max(m, 1.5m) 1.5, Min -2.5, Clamp(m, -1.5m, 1.5m) -1.5, Floor -3, Ceiling -2, Truncate (toward zero) -2.
//! cites: 2.2, 7.4
//! expect: run
//! output: 2.5
//!   1.5
//!   -2.5
//!   -1.5
//!   -3
//!   -2
//!   -2
module Conf;

public class Program
  public static method IdM(x: decimal?): decimal? return x; end IdM;
  public static method Main()
    var m: decimal? := IdM(-2.5m);
    if m <> null then
      var r: decimal := Math.Abs(m);
      Console.WriteLine(r);
      Console.WriteLine(Math.Max(m, 1.5m));
      Console.WriteLine(Math.Min(m, 1.5m));
      Console.WriteLine(Math.Clamp(m, -1.5m, 1.5m));
      Console.WriteLine(Math.Floor(m));
      Console.WriteLine(Math.Ceiling(m));
      Console.WriteLine(Math.Truncate(m));
    end;
  end Main;
end Program;
