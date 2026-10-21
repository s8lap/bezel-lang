//! case: s7_4_0059
//! doc: spec
//! section: 7.4
//! title: a catch arm reads a nullable string that the try stored null before a call that may raise
//! rule: "a fact holds at a point only when it holds on every path that reaches it" ([§7.4]). "Any
//!   exception may propagate from any method call." ([§31.3]). The arm is reached from the call
//!   that follows `s := null`, so `s` is null when the arm reads it.
//!   "every statement of the block is a possible throw point for every `catch` arm and for the
//!   `finally`" ([§7.4]).
//! cites: 7.4, 31.3
//! expect: reject
//! diagnostic: nullable
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
      Exception => Console.WriteLine(s.Length)
    end try;
  end Main;
end Program;
