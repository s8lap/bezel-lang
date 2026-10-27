//! case: l9_1_0009
//! doc: stdlib
//! section: 9.1
//! title: ToString(radix) renders a minus sign then the magnitude's digits
//! rule: stdlib §9.1: "`ToString(radix)` emits the **lower-case** digits — a
//!     `-` for a negative value followed by the magnitude's digits most
//!     significant first". The magnitude of -42 in radix 16 is `2a`.
//! cites: 9.1
//! expect: run
//! stdout: -2a
module Conf;

public class Program
  public static method Main()
    Console.WriteLine((-42).ToString(16));
  end Main;
end Program;
