//! case: l2_18_0024
//! doc: stdlib
//! section: 2.18
//! title: the NaN round trip yields NaN, equal to the original under =
//! rule: stdlib §2.18's non-finite paragraph: "`Parse` reads each of the three
//!     back (§9.2); for a NaN the round trip yields `NaN`, equal to the original
//!     under `=` (language spec §4.3.1), and does not carry its sign or payload
//!     bits". Language spec §4.3.1: "`NaN = NaN` is **true**."
//! cites: 2.18, 9.2, 4.3.1
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var x := double.Parse("NaN");
    Console.WriteLine(x = double.NaN);
  end Main;
end Program;
