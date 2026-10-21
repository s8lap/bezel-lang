//! case: s7_4_0064
//! doc: spec
//! section: 7.4
//! title: a read after a try with a catch arm is narrowed by a non-null store that follows the try
//! rule: "after `x := expr;` where `expr`'s static type is non-nullable" ([§7.4]) "`x` is narrowed until
//!   invalidated" ([§7.4]). The store `s := "z"` after the statement is non-nullable, so the read is
//!   narrowed. The point after the statement is reached through the normal completion of the block or an
//!   arm's exit, and the store after the statement holds on every such path: the block does not throw,
//!   so the arm does not run and prints nothing.
//! cites: 7.4, 31.2
//! expect: run
//! stdout: 1
//! issue: 6673
module Conf;

public class Program
  public static method Main()
    var s: string? := "x";
    try
      s := null;
      s := "y";
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end try;
    s := "z";
    Console.WriteLine(s.Length);
  end Main;
end Program;
