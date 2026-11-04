//! case: l2_2_0041
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs of a float is a double and does not implicitly return as a float
//! rule: public static pure method Abs(x: int): int; public static pure method
//!   Abs(x: long): long; public static pure method Abs(x: double): double;
//!   public static pure method Abs(x: decimal): decimal;
//! cites: 5.1, 22.6
//! expect: reject
//! diagnostic: double
//! notes: Abs(float) resolves to Abs(double) (the only applicable overload), so the body has type double. double to float is not among the implicit conversions (spec 5.1 lists float to double alone), so returning it as float is a compile error. The accepting twin is l2_2_0042, which returns double.
module Conf;

public class Program
  public static method F(f: float): float => Math.Abs(f);

  public static method Main()
    Console.WriteLine(F(-1.0f));
  end Main;
end Program;
