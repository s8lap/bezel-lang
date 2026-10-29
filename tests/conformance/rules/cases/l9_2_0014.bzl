//! case: l9_2_0014
//! doc: stdlib
//! section: 9.2
//! title: float.IeeeEquals and BitwiseEquals follow the double distinction at binary32 width
//! rule: The same `IeeeEquals`/`BitwiseEquals` distinction described for
//!   `Double` applies to `Float` with the appropriate bit-pattern width.
//! cites: 9.2, 5.1
//! expect: run
//! output: True
//!   True
//!   True
//!   False
//!   True
//!   False
module Conf;

public class Program
  public static method Main()
    var f: float := 4.0f;
    var u: uint := 4000000000u;
    var g: float := 4000000000.0f;
    var zf: float := 0.0f;
    var nzf: float := -0.0f;
    var nan: float := float.NaN;
    Console.WriteLine(float.IeeeEquals(f, f));
    Console.WriteLine(float.IeeeEquals(u, g));
    Console.WriteLine(float.BitwiseEquals(f, 4.0f));
    Console.WriteLine(float.BitwiseEquals(zf, nzf));
    Console.WriteLine(float.IeeeEquals(zf, nzf));
    Console.WriteLine(float.IeeeEquals(nan, nan));
  end Main;
end Program;
