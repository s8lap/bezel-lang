//! case: l9_6_0034
//! doc: stdlib
//! section: 9.6
//! title: a string's Remove window counts code points in a four-byte-wide string
//! rule: stdlib §9.6 (v0.345) "The window members: `Substring`, `Remove` and `Insert`":
//!   "Positions and lengths count code points (`Length`)" — the window is over the code-point
//!   sequence whatever the string's internal width is.
//! cites: 9.6
//! expect: run
//! stdout: abc
module Conf;

public class Program
  public static method Main()
    var s: string := "日" + "abc";
    Console.WriteLine(s.Remove(0, 1));
  end Main;
end Program;
