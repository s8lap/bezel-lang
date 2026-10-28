//! case: l2_18_0031
//! doc: stdlib
//! section: 2.18
//! title: a format spec on a string passed to ToString is a compile error
//! rule: stdlib §2.18 (v0.345), specifier table: "| `bool`, `char`, `string` | *(none)* | a
//!   format spec on these is a compile error" — `string` declares no `ToString(format)` form.
//! cites: 2.18, 9.6
//! expect: reject
//! diagnostic: ToString
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("abc".ToString("F4"));
  end Main;
end Program;
