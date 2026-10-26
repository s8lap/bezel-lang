//! case: l2_18_0019
//! doc: stdlib
//! section: 2.18
//! title: the D specifier of an unsigned value keeps its value
//! rule: stdlib §2.18's integer row: "`D`/`Dn` decimal (n = min digits,
//!     zero-padded)". The `D` spelling of 4000000000 is its decimal text.
//! cites: 2.18, 9.1
//! expect: run
//! stdout: 4000000000
//! issue: 7265
module Conf;

public class Program
  public static method Main()
    var u: uint := 4000000000U;
    Console.WriteLine(u.ToString("D"));
  end Main;
end Program;
