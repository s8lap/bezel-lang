//! case: l2_2_0042
//! doc: stdlib
//! section: 2.2
//! title: Math.Abs of a float returns as a double
//! rule: public static pure method Abs(x: int): int; public static pure method
//!   Abs(x: long): long; public static pure method Abs(x: double): double;
//!   public static pure method Abs(x: decimal): decimal;
//! cites: 5.1, 22.6
//! expect: run
//! stdout: 1.5
//! notes: Accepting twin of l2_2_0041: the same body with a double return type. Abs(float) is Abs(double), so the call is double and returns unchanged; the value -1.5 is exact in float, so it renders 1.5.
module Conf;

public class Program
  public static method F(f: float): double => Math.Abs(f);

  public static method Main()
    Console.WriteLine(F(-1.5f));
  end Main;
end Program;
