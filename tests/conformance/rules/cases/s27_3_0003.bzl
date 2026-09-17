//! case: s27_3_0003
//! doc: spec
//! section: 27.3
//! title: a return-type ascription is a written integer target for a target-typed constant
//! rule: The return-type ascription behaves exactly as a method's declared
//!   return type; it is a written integer target for §4.2's target-typed
//!   integer constant — `lambda(): long => 3_000_000_000` is admitted by
//!   §5.1's constant narrowing.
//! cites: 27.3, 4.2, 5.1
//! expect: run
//! stdout: 3000000000
module Conf;

public class Program
  public static method Main()
    var big: () -> long := lambda(): long => 3_000_000_000;
    Console.WriteLine($"{big()}");
  end Main;
end Program;
