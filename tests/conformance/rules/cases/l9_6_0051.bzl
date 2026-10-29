//! case: l9_6_0051
//! doc: stdlib
//! section: 9.6
//! title: string CompareTo binds named ignoreCase before other and compares case-folded or ordinal accordingly
//! rule: public pure method CompareTo(other: string, ignoreCase: bool): int;
//! cites: 9.6, 22.5
//! expect: run
//! output: 0
//!   True
module Conf;

public class Program
  public static method Main()
    var s := "hello world";
    Console.WriteLine(s.CompareTo(ignoreCase := true, other := "HELLO WORLD"));
    Console.WriteLine(s.CompareTo(ignoreCase := false, other := "HELLO WORLD") > 0);
  end Main;
end Program;
