//! case: l9_6_0020
//! doc: stdlib
//! section: 9.6
//! title: string.Empty is not a declared member of String
//! rule: The `String` member list declares no `Empty` member — the
//!   empty-string member it declares is `IsEmpty` — so a static reference to
//!   `string.Empty` names no declared member and is a compile error.
//! cites: 9.6, 22.6
//! expect: reject
//! diagnostic: Empty
module Conf;

public class Program
  public static method Main(): int
    var t := string.Empty;
    Console.WriteLine(t.Length);
    return 0;
  end Main;
end Program;
