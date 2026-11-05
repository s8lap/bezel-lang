//! case: l2_18_0903
//! doc: stdlib
//! section: 2.18
//! title: a decimal percent with Pn digits rounds a midpoint at the nth decimal half-to-even
//! rule: For a `decimal` operand, digit reduction under `F`/`Fn`, `P`/`Pn`, and `E`/`En` rounds a
//!   midpoint **half-to-even** (banker's rounding)
//! cites: 2.18, 9.3
//! expect: run
//! output: 1.2%
//!   1.4%
//!   0.12%
//!   0.14%
//!   100.0%
//! issue: 2253
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{0.0125m:P1}");
    Console.WriteLine($"{0.0135m:P1}");
    Console.WriteLine($"{0.00125m:P2}");
    Console.WriteLine($"{0.00135m:P2}");
    Console.WriteLine($"{0.9995m:P1}");
  end Main;
end Program;
