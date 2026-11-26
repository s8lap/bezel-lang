//! case: s22_2_3602
//! doc: spec
//! section: 22.2
//! title: a procedure's arrow body over a unit call draws nothing
//! rule: On a method with **no declared return type**, an expression body is the one-statement block
//!   `expression;` it replaces, and its result is under §21.9's expression-statement regime exactly as that
//!   statement's would be: a `()` result draws nothing (`method G() => Log("x");`); any other result draws
//!   the discard warning (`method F() => 5;`) … and a non-trivial-dispose class, interface-typed or
//!   `Task`-family result is §21.9's hard error
//! cites: 22.2, 21.9, 27.3
//! expect: quiet
//! issue: 9246
//! notes: Twin of s22_2_3601: the body's result is `()`.
module Conf;

public class Program
  public static method Log(s: string) => Console.WriteLine(s);
  public static method G() => Program.Log("x");
end Program;
