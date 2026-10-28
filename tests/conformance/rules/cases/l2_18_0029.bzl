//! case: l2_18_0029
//! doc: stdlib
//! section: 2.18
//! title: a format spec on a bool in an interpolation hole is a compile error
//! rule: stdlib §2.18 (v0.345), specifier table: "| `bool`, `char`, `string` | *(none)* | a
//!   format spec on these is a compile error" — the interpolation hole dispatches its spec to the
//!   same vocabulary.
//! cites: 2.18
//! expect: reject
//! diagnostic: format
module Conf;

public class Program
  public static method Main()
    Console.WriteLine($"{true:F4}");
  end Main;
end Program;
