//! case: s33_2_0002
//! doc: spec
//! section: 33.2
//! title: Main(args) with no return type qualifies
//! rule: `public static method Main(args: Array<string>)` is one of the eight
//!   accepted entry-point signatures.
//! cites: 33.1, C.12
//! expect: run
//! stdout: sig1
module Conf;

public class Program
  public static method Main(args: Array<string>)
    Console.WriteLine("sig1");
  end Main;
end Program;
