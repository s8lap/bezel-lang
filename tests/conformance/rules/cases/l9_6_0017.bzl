//! case: l9_6_0017
//! doc: stdlib
//! section: 9.6
//! title: a call to Substring supplying three arguments matches no declared form
//! rule: public pure method Substring(startIndex: int): string; public pure
//!   method Substring(startIndex: int, length: int): string; — the declared
//!   forms take one or two arguments, so a call supplying three binds neither;
//!   arity filtering leaves no candidate and the call is a compile error.
//! cites: 9.6, 22.5, 22.6
//! expect: reject
//! diagnostic: Substring
module Conf;

public class Program
  public static method Main(): int
    var t := "abcdef".Substring(0, 2, 3);
    Console.WriteLine(t.Length);
    return 0;
  end Main;
end Program;
