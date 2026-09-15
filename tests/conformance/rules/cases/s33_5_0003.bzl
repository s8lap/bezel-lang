//! case: s33_5_0003
//! doc: spec
//! section: 33.5
//! title: two classes each declaring a qualifying Main is rejected
//! rule: A program is ill-formed if more than one method qualifies as the
//!   entry point. This applies across classes (multiple classes each
//!   declaring a qualifying `Main`). Without disambiguation, the compiler
//!   produces an error listing every qualifying `Main` it found.
//! cites: 33.1, C.12
//! expect: reject
//! diagnostic: qualifies
module Conf;

public class ProgramOne
  public static method Main()
    Console.WriteLine("one");
  end Main;
end ProgramOne;

public class ProgramTwo
  public static method Main()
    Console.WriteLine("two");
  end Main;
end ProgramTwo;
