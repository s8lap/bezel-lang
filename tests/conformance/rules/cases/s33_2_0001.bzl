//! case: s33_2_0001
//! doc: spec
//! section: 33.2
//! title: Main() with no parameters and no return type qualifies
//! rule: `public static method Main()` is one of the eight accepted
//!   entry-point signatures.
//! cites: 33.1, C.12
//! expect: run
//! stdout: sig0
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("sig0");
  end Main;
end Program;
