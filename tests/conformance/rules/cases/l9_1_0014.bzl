//! case: l9_1_0014
//! doc: stdlib
//! section: 9.1
//! title: the no-format ToString emits the text ToString(10) emits
//! rule: stdlib §9.1: "The no-format `ToString()` ... emits the text
//!     `ToString(10)` emits: `(-42).ToString()` is `"-42"`".
//! cites: 9.1
//! expect: run
//! stdout: -42
module Conf;

public class Program
  public static method Main()
    Console.WriteLine((-42).ToString());
  end Main;
end Program;
