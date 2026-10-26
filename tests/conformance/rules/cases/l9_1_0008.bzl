//! case: l9_1_0008
//! doc: stdlib
//! section: 9.1
//! title: ToString(radix) emits the lower-case digits
//! rule: stdlib §9.1: "`ToString(radix)` emits the **lower-case** digits".
//!     The radix alphabet of radix 16 is `0`–`9` with `A`–`F`, and the
//!     rendering of 255 is the lower-case `ff`.
//! cites: 9.1
//! expect: run
//! stdout: ff
module Conf;

public class Program
  public static method Main()
    Console.WriteLine((255).ToString(16));
  end Main;
end Program;
