//! case: s7_4_0115
//! doc: spec
//! section: 7.4
//! title: a declaration whose initializer is a non-null literal narrows the local for a member call
//! rule: The knowledge is established by ... assignment: ... and from a
//!   declaration whose initializer is such an `expr` — `var x: T? := expr;` or
//!   `const x: T? := expr;` — which is the declaration's assignment.
//! cites: 7.4
//! expect: run
//! stdout: 5
module Conf;

public class Program
  public static method Main()
    var s: string? := "a,b,c";
    var t := s.Trim();
    Console.WriteLine(t.Length);
  end Main;
end Program;
