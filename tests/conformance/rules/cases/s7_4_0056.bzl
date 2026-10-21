//! case: s7_4_0056
//! doc: spec
//! section: 7.4
//! title: a catch arm that returns leaves only the non-null try path to the read
//! rule: "a fact holds at a point only when it holds on every path that reaches it" ([§7.4]). "`return`
//!   exits the nearest enclosing callable" ([§21.7]), so the catch arm reaches no point after the
//!   try; the only path to the read is the try block, which stores a non-null value last.
//! cites: 7.4, 21.7
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
      s := null;
      Boom(false);
      s := "y";
    catch
      Exception => return;
    end try;
    Console.WriteLine(s.Length);
  end Main;
end Program;
