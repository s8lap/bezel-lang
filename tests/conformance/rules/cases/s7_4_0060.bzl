//! case: s7_4_0060
//! doc: spec
//! section: 7.4
//! title: a catch arm reads a non-null string that the try stored before a call that may raise
//! rule: "a fact holds at a point only when it holds on every path that reaches it" ([§7.4]). "after `x
//!   := expr;` where `expr`'s static type is non-nullable" ([§7.4]) "`x` is narrowed until
//!   invalidated" ([§7.4]). The store before the call is non-nullable and reaches the arm on every
//!   path, so the arm's read is narrowed.
//! cites: 7.4, 31.3
//! expect: run
//! stdout: 1
//! issue: 6673
module Conf;

public class Program
  public static method Boom(f: bool)
    if f then throw new InvalidOperationException("x"); end;
  end Boom;

  public static method Main()
    var s: string? := "x";
    try
      s := "z";
      Boom(true);
      s := "y";
    catch
      Exception => Console.WriteLine(s.Length)
    end try;
  end Main;
end Program;
