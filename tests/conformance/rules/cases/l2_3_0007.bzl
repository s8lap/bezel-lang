//! case: l2_3_0007
//! doc: stdlib
//! section: 2.3
//! title: Convert.ToString of a long argument resolves to the long overload and renders the value
//! rule: stdlib §2.3 declares `ToString(value: long): string` among the one-argument
//!     forms, and states that "where that selects an overload the text is the
//!     converted value's". A `long` argument is an exact match for the `long`
//!     overload (§22.6), so the call renders the value's own no-format text.
//! cites: 2.3, 22.6
//! expect: run
//! stdout: 255
//! issue: 7218
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Convert.ToString(255L));
  end Main;
end Program;
