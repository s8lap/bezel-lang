//! case: l2_18_0906
//! doc: stdlib
//! section: 2.18
//! title: decimal F2, Round and P2 agree on one midpoint rounding
//! rule: For a `decimal` operand, digit reduction under `F`/`Fn`, `P`/`Pn`, and `E`/`En` rounds a
//!   midpoint **half-to-even** (banker's rounding)
//! cites: 2.18, 9.3
//! expect: run
//! output: 0.12
//!   0.14
//!   0.12
//!   0.12%
//! issue: 2253
module Conf;

public class Program
  public static method Main()
    var a := 0.125m;
    var b := 0.135m;
    Console.WriteLine($"{a:F2}");
    Console.WriteLine($"{b:F2}");
    Console.WriteLine(a.Round(2));
    Console.WriteLine($"{0.00125m:P2}");
  end Main;
end Program;
