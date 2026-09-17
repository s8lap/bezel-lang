//! case: s10_4_0004
//! doc: spec
//! section: 10.4
//! title: NameOf yields the first-declared identifier for a shared value
//! rule: Under duplicate constant values, `NameOf` yields the first-declared
//!   constant's identifier for a shared value.
//! cites: 10.4, 10.2
//! expect: run
//! stdout: Legacy
module Conf;

enum Level
  Legacy := 1, Current := 1, Retired := 2;
end Level;

public class Program
  public static method Main()
    Console.WriteLine(Level.NameOf(Level.Current));
  end Main;
end Program;
