//! case: s33_2_0003
//! doc: spec
//! section: 33.2
//! title: Main(): int qualifies
//! rule: `public static method Main(): int` is one of the eight accepted
//!   entry-point signatures.
//! cites: 33.1, 33.3, C.12
//! expect: run
//! stdout: sig2
module Conf;

public class Program
  public static method Main(): int
    Console.WriteLine("sig2");
    return 0;
  end Main;
end Program;
