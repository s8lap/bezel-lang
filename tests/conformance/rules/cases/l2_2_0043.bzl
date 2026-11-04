//! case: l2_2_0043
//! doc: stdlib
//! section: 2.2
//! title: a Math.Abs of a float picks the double overload of a user method
//! rule: public static pure method Abs(x: int): int; public static pure method
//!   Abs(x: long): long; public static pure method Abs(x: double): double;
//!   public static pure method Abs(x: decimal): decimal;
//! cites: 5.1, 22.6
//! expect: run
//! stdout: double
//! notes: Abs(float) has type double, so overload resolution among the user methods sees a double argument: W(double) is exact and W(float) is not applicable (double to float is not implicit, spec 5.1). The result is W(double).
module Conf;

public class Program
  public static method W(x: float): string => "float";
  public static method W(x: double): string => "double";

  public static method Main()
    var f: float := -2.5f;
    Console.WriteLine(W(Math.Abs(f)));
  end Main;
end Program;
