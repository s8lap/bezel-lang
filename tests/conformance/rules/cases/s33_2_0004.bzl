//! case: s33_2_0004
//! doc: spec
//! section: 33.2
//! title: Main(args): int qualifies
//! rule: `public static method Main(args: Array<string>): int` is one of the
//!   eight accepted entry-point signatures.
//! cites: 33.1, 33.3, C.12
//! expect: run
//! stdout: sig3
module Conf;

public class Program
  public static method Main(args: Array<string>): int
    Console.WriteLine("sig3");
    return 0;
  end Main;
end Program;
