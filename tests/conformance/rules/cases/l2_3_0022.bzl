//! case: l2_3_0022
//! doc: stdlib
//! section: 2.3
//! title: a named Convert.ToString binds each parameter by name and renders the radix form
//! rule: stdlib §2.3: "`ToString(value, toBase)` returns the text
//!     `value.ToString(toBase)` returns (§9.1)". §22.5: "Named arguments may
//!     appear in any order after positional arguments. Each supplied argument
//!     binds exactly one parameter". The named spelling binds `value` and
//!     `toBase` to the two parameters, and the rendered text is the radix form.
//! cites: 2.3, 9.1, 22.5
//! expect: run
//! stdout: ff
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Convert.ToString(value := 255, toBase := 16));
  end Main;
end Program;
