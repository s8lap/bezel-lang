//! case: l2_3_0017
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString of a bool renders True
//! rule: stdlib §2.3: "Each one-argument `ToString` returns the text the value
//!     it receives renders as under its own no-format `ToString()`", and the
//!     section's own example: "`Convert.ToString(true)` is `"True"`". A `bool`
//!     renders `True`/`False` (§8.5).
//! cites: 2.3, 8.5
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Convert.ToString(true));
  end Main;
end Program;
