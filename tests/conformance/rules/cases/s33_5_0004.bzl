//! case: s33_5_0004
//! doc: spec
//! section: 33.5
//! title: only one class declaring a qualifying Main resolves cleanly
//! rule: An instance method named `Main` does not qualify; making the second
//!   class's `Main` an instance method removes the cross-class conflict.
//! cites: 33.1, 33.2
//! expect: run
//! stdout: one
module Conf;

public class ProgramOne
  public static method Main()
    Console.WriteLine("one");
  end Main;
end ProgramOne;

public class ProgramTwo
  public method Main()
    Console.WriteLine("two");
  end Main;
end ProgramTwo;
