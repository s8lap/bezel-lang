//! case: l9_1_0010
//! doc: stdlib
//! section: 9.1
//! title: ToString(radix) renders zero as a single 0
//! rule: stdlib §9.1: "`ToString(radix)` emits the **lower-case** digits — ...
//!     a single `0` for zero, never a leading zero otherwise".
//! cites: 9.1
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    Console.WriteLine((0).ToString(16));
  end Main;
end Program;
