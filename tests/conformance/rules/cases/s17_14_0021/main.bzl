//! case: s17_14_0021
//! doc: spec
//! section: 17.14
//! title: an imported const whose value does not fit the target's range is a compile error
//! rule: spec §17.14: a reference to a `const` member is a constant expression,
//!     and §5.1: "A constant whose value does not fit the target's range is a
//!     **compile error** at the conversion site — never a silent truncation".
//!     The imported `uint` constant's value 4000000000 does not fit `int`'s
//!     range, so the conversion at the radix parameter is a compile error.
//! cites: 17.14, 5.1
//! expect: reject
//! diagnostic: range
module App;

import Lib;

public class Program
  public static method Main()
    Console.WriteLine(int.Parse("ff", Cfg.K));
  end Main;
end Program;
