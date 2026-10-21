//! case: s7_4_0055
//! doc: spec
//! section: 7.4
//! title: a catch arm that completes normally reads a nullable string after a call that may raise
//! rule: "a fact holds at a point only when it holds on every path that reaches it" ([§7.4]). "Any
//!   exception may propagate from any method call." ([§31.3]). The catch arm completes normally on
//!   the path where `s` was set to null before the call, so the read after the try sees a null `s`.
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
      Exception => Console.WriteLine("recovered")
    end try;
    Console.WriteLine(s.Length);
  end Main;
end Program;
