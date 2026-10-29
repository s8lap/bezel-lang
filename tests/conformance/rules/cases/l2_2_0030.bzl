//! case: l2_2_0030
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs/Min/Clamp accept a double? narrowed by an early return
//! rule: stdlib §2.2 declares Abs(x: double), Min<T>, Clamp<T>. §7.4 (v0.350): narrowing is flow-sensitive and "a null test ... `x <> null` ... narrowing the true branch"; a narrowed nullable local has the non-nullable type, so a stdlib parameter of the underlying type accepts it. After `if e = null then return; end;` the surviving path is narrowed ("an early exit", §7.4), so the calls type-check and compute -2.5 -> 2.5, Min(-2.5, 0.5) -> -2.5, Clamp(-2.5, -1.0, 1.0) -> -1.
//! cites: 2.2, 7.4
//! expect: run
//! output: 2.5
//!   -2.5
//!   -1
module Conf;

public class Program
  public static method IdD(x: double?): double? return x; end IdD;
  public static method Main()
    var e: double? := IdD(-2.5);
    if e = null then return; end;
    var r: double := Math.Abs(e);
    Console.WriteLine(r);
    Console.WriteLine(Math.Min(e, 0.5));
    Console.WriteLine(Math.Clamp(e, -1.0, 1.0));
  end Main;
end Program;
