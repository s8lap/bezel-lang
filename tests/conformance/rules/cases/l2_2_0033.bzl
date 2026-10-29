//! case: l2_2_0033
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs and DivRem accept an int? narrowed non-null
//! rule: stdlib §2.2 declares Abs(x: int) and DivRem(a: int, b: int, out remainder: int) "returns the quotient". §7.4 (v0.350): narrowing is flow-sensitive and "a null test ... `x <> null` ... narrowing the true branch"; a narrowed nullable local has the non-nullable type, so a stdlib parameter of the underlying type accepts it. For -7: Abs 7; DivRem(-7, 2) quotient -3 (truncated toward zero, §6.1) with remainder -1.
//! cites: 2.2, 7.4, 6.1
//! expect: run
//! output: 7
//!   -3
//!   -1
module Conf;

public class Program
  public static method IdI(x: int?): int? return x; end IdI;
  public static method Main()
    var l: int? := IdI(-7);
    if l <> null then
      var r: int := Math.Abs(l);
      Console.WriteLine(r);
      var q: int := 0;
      Console.WriteLine(Math.DivRem(l, 2, out q));
      Console.WriteLine(q);
    end;
  end Main;
end Program;
