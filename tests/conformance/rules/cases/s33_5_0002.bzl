//! case: s33_5_0002
//! doc: spec
//! section: 33.5
//! title: only one overload qualifying resolves the entry point cleanly
//! rule: A class may declare at most one qualifying `Main`; making the second
//!   overload non-qualifying (here, `private`) removes the conflict.
//! cites: 33.1, 33.2
//! expect: run
//! stdout: a
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("a");
  end Main;

  private static method Main(args: Array<string>)
    Console.WriteLine("b");
  end Main;
end Program;
