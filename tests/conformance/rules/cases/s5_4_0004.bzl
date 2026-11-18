//! case: s5_4_0004
//! doc: spec
//! section: 5.4
//! title: FromUnderlyingOrNull returns null where the checked cast would fault
//! rule: Code for which an undeclared value is an expected input — a decoder
//!   reading values from outside the program — branches instead of catching:
//!   every enum carries a compiler-provided static member
//!   `E.FromUnderlyingOrNull(value: U): E?` (§10.4), the non-throwing form,
//!   which returns `null` where the cast would raise.
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
