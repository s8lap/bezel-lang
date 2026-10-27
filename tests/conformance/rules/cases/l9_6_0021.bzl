//! case: l9_6_0021
//! doc: stdlib
//! section: 9.6
//! title: an instance member named Empty is not a declared member of String
//! rule: The `String` member list declares no `Empty` member — the
//!   empty-string member it declares is `IsEmpty` — so a member access
//!   `.Empty` on a string names no declared member and is a compile error.
//! cites: 9.6, 22.6
//! expect: reject
//! diagnostic: Empty
module Conf;

public class Program
  public static method Main(): int
    var s := "abc";
    Console.WriteLine(s.Empty.Length);
    return 0;
  end Main;
end Program;
