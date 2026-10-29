//! case: l2_2_0031
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs/Max/Floor accept a double? narrowed on the else branch of `is null`
//! rule: stdlib §2.2 Abs/Max/Floor/Sqrt over double. §7.4 (v0.350): narrowing is flow-sensitive and "a null test ... `x <> null` ... narrowing the true branch"; a narrowed nullable local has the non-nullable type, so a stdlib parameter of the underlying type accepts it. "x = null or x is null (narrowing the *false* branch)": the else branch of `e is null` has `e` non-null: Abs(-2.5) -> 2.5, Max(-2.5, -9.0) -> -2.5, Floor(-2.5) -> -3.
//! cites: 2.2, 7.4
//! expect: run
//! output: 2.5
//!   -2.5
//!   -3
module Conf;

public class Program
  public static method IdD(x: double?): double? return x; end IdD;
  public static method Main()
    var e: double? := IdD(-2.5);
    if e is null then
      Console.WriteLine(0);
    else
      Console.WriteLine(Math.Abs(e));
      Console.WriteLine(Math.Max(e, -9.0));
      Console.WriteLine(Math.Floor(e));
    end;
  end Main;
end Program;
