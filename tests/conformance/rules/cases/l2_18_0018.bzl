//! case: l2_18_0018
//! doc: stdlib
//! section: 2.18
//! title: the no-format ToString of an unsigned value keeps its value
//! rule: stdlib §9.1: "The no-format `ToString()` ... emits the text
//!     `ToString(10)` emits", and §2.18's integer row gives the decimal
//!     spelling of an integer. 4000000000 is a `uint` value and its decimal
//!     text is 4000000000 — the value, not the reinterpretation of its bits.
//! cites: 2.18, 9.1
//! expect: run
//! stdout: 4000000000
module Conf;

public class Program
  public static method Main()
    var u: uint := 4000000000U;
    Console.WriteLine(u.ToString());
  end Main;
end Program;
