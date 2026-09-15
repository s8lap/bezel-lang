//! case: s4_4_0002
//! doc: spec
//! section: 4.4
//! title: decimal arithmetic exactly represents finite decimal fractions
//! rule: High-precision decimal floating-point ... Exact representation of
//!   finite decimal fractions.
//! cites: 4.4
//! expect: run
//! stdout: 0.3
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{0.1m + 0.2m}");
  end Main;
end Program;
