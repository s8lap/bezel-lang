//! case: l2_18_0007
//! doc: stdlib
//! section: 2.18
//! title: a temporal pattern's non-specifier characters are literals
//! rule: In a temporal pattern, every character that does not begin one of
//!   the specifiers is a literal, copied to the output verbatim — T, Z, -,
//!   :, space, and any other non-specifier character alike.
//! cites: 2.18
//! expect: run
//! stdout: 2024-03-05T09:07:02Z
module Conf;

public class Program
  public static method Main()
    var d := new DateTime(2024, 3, 5, 9, 7, 2);
    Console.WriteLine(d.ToString("yyyy-MM-ddTHH:mm:ssZ"));
  end Main;
end Program;
