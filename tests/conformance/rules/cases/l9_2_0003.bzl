//! case: l9_2_0003
//! doc: stdlib
//! section: 9.2
//! title: double.Parse admits the spellings the input grammar lists
//! rule: **Input grammar.** `Parse` accepts an optional sign (`+` or `-`), then one
//!   or more of the digits `0`–`9`, then optionally a `.` followed by one or
//!   more digits, then optionally an exponent — `e` or `E`, an optional sign,
//!   and one or more digits — and nothing else: `"5"`, `"+5"`, `"-0.25"`,
//!   `"007.5"` and `"6.02E+23"` are admitted.
//! cites: 9.2
//! expect: run
//! output: 5 -> 5
//!   +5 -> 5
//!   -0.25 -> -0.25
//!   007.5 -> 7.5

module Conf;

public class Program
  public static method Main()
    Show("5");
    Show("+5");
    Show("-0.25");
    Show("007.5");
  end Main;

  private static method Show(s: string)
    var v := double.Parse(s);
    Console.WriteLine(s + " -> " + v.ToString());
  end Show;
end Program;
