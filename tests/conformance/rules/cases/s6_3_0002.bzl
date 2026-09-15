//! case: s6_3_0002
//! doc: spec
//! section: 6.3
//! title: 0.0 / 0.0 produces NaN
//! rule: Operations involving NaN propagate NaN. Division by zero produces
//!   +/-Infinity or NaN. No exception is raised.
//! cites: 6.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var z: double := 0.0;
    var r := z / z;
    Console.WriteLine(double.IsNaN(r));
  end Main;
end Program;
