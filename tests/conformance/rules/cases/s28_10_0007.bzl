//! case: s28_10_0007
//! doc: spec
//! section: 28.10
//! title: using var accepts a runtime-managed right-hand side, where disposal at scope end is a no-op
//! rule: using var accepts a value-type or runtime-managed right-hand side;
//!   for ordinary value types and runtime-managed types the disposal at
//!   scope end is a no-op, and the only effect is the reassignability
//!   restriction.
//! cites: 28.10, 28.9
//! expect: run
//! stdout: hello
module Conf;

public class Program
  public static method Main()
    using var s := "hello";
    Console.WriteLine(s);
  end Main;
end Program;
