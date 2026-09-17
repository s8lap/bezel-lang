//! case: s5_4_0004
//! doc: spec
//! section: 5.4
//! title: FromUnderlyingOrNull returns null where the checked cast would fault
//! rule: Boundary code for which an undeclared value is an expected input
//!   branches instead of catching: every enum carries a compiler-provided
//!   static member E.FromUnderlyingOrNull(value: U): E?, which returns null
//!   where the cast would fault.
//! cites: 5.4
//! expect: run
//! stdout: [null]
module Conf;

public enum Priority
  Low, Medium, High;
end Priority;

public class Program
  public static method Main()
    var p := Priority.FromUnderlyingOrNull(5);
    Console.WriteLine($"[{p}]");
  end Main;
end Program;
