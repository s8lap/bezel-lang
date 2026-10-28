//! case: l2_18_0028
//! doc: stdlib
//! section: 2.18
//! title: a format spec on a bool is a compile error
//! rule: stdlib §2.18 (v0.345), specifier table: "| `bool`, `char`, `string` | *(none)* | a
//!   format spec on these is a compile error".
//! cites: 2.18
//! expect: reject
//! diagnostic: format
module Conf;

public class Program
  public static method Main()
    var b := true;
    Console.WriteLine(b.ToString("F4"));
  end Main;
end Program;
