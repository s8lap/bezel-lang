//! case: s32_10_0004
//! doc: spec
//! section: 32.10
//! title: an int argument at a long extern parameter crosses as a long
//! rule: The Bezel compiler guarantees: … Arguments passed to an `extern` method are
//!   of the declared types.
//! cites: 32.10, 5.1
//! expect: run
//! stdout: 7
//! notes: the int local converts implicitly to the declared long before the call; a
//!   compiler passing the 32-bit value in a 64-bit slot leaves the upper half
//!   unspecified.
module Conf;

extern "libc"
  public method labs(x: long): long;
end;

public class Program
  public static method Main()
    var x: int := -7;
    Console.WriteLine(labs(x));
  end Main;
end Program;
