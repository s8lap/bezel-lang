//! case: l2_18_0025
//! doc: stdlib
//! section: 2.18
//! title: a NaN from an operation renders NaN like the NaN constant
//! rule: stdlib §2.18's non-finite paragraph: "Every NaN renders `NaN`,
//!     whatever its sign bit and payload." A NaN produced by an operation —
//!     here `Math.Sqrt` of a negative argument, IEEE 754 per language spec
//!     §6.3 — renders the same name as the `double.NaN` constant.
//! cites: 2.18, 2.2, 6.3
//! expect: run
//! stdout: NaN
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Math.Sqrt(-1.0).ToString());
  end Main;
end Program;
