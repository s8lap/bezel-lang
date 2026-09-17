//! case: s5_2_0005
//! doc: spec
//! section: 5.2
//! title: narrowing a double beyond float's finite range yields Infinity and never raises
//! rule: float(d) for double d rounds to the nearest float; a magnitude beyond
//!   float's finite range yields +/-Infinity and never raises — floating-point
//!   narrowing follows floating-point arithmetic's own overflow behavior.
//! cites: 5.2, 6.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var d: double := double.MaxValue;
    var f := float(d);
    Console.WriteLine(float.IsPositiveInfinity(f));
  end Main;
end Program;
