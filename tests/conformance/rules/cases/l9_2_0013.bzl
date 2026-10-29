//! case: l9_2_0013
//! doc: stdlib
//! section: 9.2
//! title: double.IeeeEquals and BitwiseEquals widen integer and float arguments before comparing
//! rule: `IeeeEquals` returns `false` for any pair involving `NaN`;
//!   `BitwiseEquals` returns `true` only when the two `double` values have
//!   identical bit patterns; it distinguishes `+0.0` from `-0.0`. Both take
//!   `double` parameters, so a `uint` or `float` argument converts implicitly
//!   (spec 5.1) and a value above 2^31 keeps its sign.
//! cites: 9.2, 5.1
//! expect: run
//! output: True
//!   True
//!   True
//!   True
//!   True
//!   True
//!   False
//!   False
//!   True
module Conf;

public class Program
  public static method Main()
    var u: uint := 4000000000u;
    var f: float := 4.0f;
    var g: float := 4000000000.0f;
    var d: double := 4000000000.0;
    var z: double := 0.0;
    var nz: double := -0.0;
    var nan: double := double.NaN;
    Console.WriteLine(double.IeeeEquals(u, g));
    Console.WriteLine(double.IeeeEquals(f, 4.0));
    Console.WriteLine(double.IeeeEquals(200u, 200.0f));
    Console.WriteLine(double.IeeeEquals(200.0f, 200.0));
    Console.WriteLine(double.IeeeEquals(d, d));
    Console.WriteLine(double.IeeeEquals(z, nz));
    Console.WriteLine(double.BitwiseEquals(z, nz));
    Console.WriteLine(double.IeeeEquals(nan, nan));
    Console.WriteLine(double.BitwiseEquals(g, d));
  end Main;
end Program;
