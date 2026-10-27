//! case: l2_3_0005
//! doc: stdlib
//! section: 2.3
//! title: a reordered named argument on a Convert member binds to the parameter it names
//! rule: stdlib §2.3 declares `ToString(value: int, toBase: int): string` and
//!   `ToString(value: int): string`, and states that `ToString(value, toBase)`
//!   returns the text `value.ToString(toBase)` returns (§9.1), whose radix digits
//!   are lower-case. §22.5: naming an argument changes which parameter it binds
//!   to - so `ToString(toBase := 16, value := 255)` is `ToString(value: 255,
//!   toBase: 16)` and renders "ff", while the one-argument form renders the
//!   value's own no-format text, "255".
//! cites: 2.3, 22.5, 9.1
//! expect: run
//! output: ff
//!   255
//! issue: 7199
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(Convert.ToString(toBase := 16, value := 255));
    Console.WriteLine(Convert.ToString(value := 255));
  end Main;
end Program;
