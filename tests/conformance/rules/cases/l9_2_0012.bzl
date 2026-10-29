//! case: l9_2_0012
//! doc: stdlib
//! section: 9.2
//! title: double.BitwiseEquals compares bit patterns of widened float operands and tells -0.0 from +0.0
//! rule: `BitwiseEquals` returns `true` only when the two `double` values have
//!   identical bit patterns; it distinguishes `+0.0` from `-0.0`.
//! cites: 9.2, 5.1
//! expect: run
//! output: False
//!   False
//!   False
//!   True
module Conf;

public class Program
  public static method Main()
    var f: float := 0.1f;
    var d: double := 0.1;
    var zf: float := 0.0f;
    var nzd: double := -0.0;
    Console.WriteLine(double.BitwiseEquals(f, d));
    Console.WriteLine(double.IeeeEquals(f, d));
    Console.WriteLine(double.BitwiseEquals(zf, nzd));
    Console.WriteLine(double.BitwiseEquals(-zf, nzd));
  end Main;
end Program;
