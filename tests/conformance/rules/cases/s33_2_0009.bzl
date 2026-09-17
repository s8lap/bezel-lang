//! case: s33_2_0009
//! doc: spec
//! section: 33.2
//! title: a private static Main overload does not conflict with a qualifying one
//! rule: `Main` must be `public` and `static`. A signature outside the eight
//!   accepted forms does not qualify as an entry point; a method named
//!   `Main` with a non-qualifying signature is an ordinary method.
//! cites: 33.1, 33.5
//! expect: run
//! stdout: qualifying-ran
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("qualifying-ran");
  end Main;

  private static method Main(x: int)
    Console.WriteLine("never-called");
  end Main;
end Program;
