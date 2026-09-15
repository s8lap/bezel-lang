//! case: s6_3_0003
//! doc: spec
//! section: 6.3
//! title: a nonzero double divided by zero produces Infinity rather than raising
//! rule: Division by zero produces +/-Infinity or NaN. No exception is raised.
//! cites: 6.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var n: double := 1.0;
    var z: double := 0.0;
    var r := n / z;
    Console.WriteLine(double.IsPositiveInfinity(r));
  end Main;
end Program;
