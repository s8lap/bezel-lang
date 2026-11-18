//! case: s7_7_0022
//! doc: spec
//! section: 7.7
//! title: ??= over a non-nullable string target warns that the assignment never happens
//! rule: "`s ?? "b"` over `s: string` is well-typed. Such an expression draws a warning that the right operand is never evaluated (Evaluation, below), and `x ??= e` over a non-nullable target (§6.6) one that the assignment never happens."
//!   Warning twin of s7_7_0023: the declared type is the one difference.
//! cites: 7.7, 6.6
//! expect: warn
//! diagnostic: never
//! issue: 8596
module Conf;

public class Program
  public static method Main()
    var s: string := "a";
    s ??= "b";
    Console.WriteLine(s);
  end Main;
end Program;
