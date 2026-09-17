//! case: s6_3_0001
//! doc: spec
//! section: 6.3
//! title: floating-point overflow produces Infinity rather than raising
//! rule: float and double arithmetic follows IEEE 754. Overflow produces
//!   +/-Infinity. No exception is raised.
//! cites: 6.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var d: double := double.MaxValue;
    var r := d * 2.0;
    Console.WriteLine(double.IsPositiveInfinity(r));
  end Main;
end Program;
