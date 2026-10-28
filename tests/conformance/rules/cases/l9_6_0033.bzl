//! case: l9_6_0033
//! doc: stdlib
//! section: 9.6
//! title: a string's Remove window counts code points, not bytes
//! rule: stdlib §9.6 (v0.345) "The window members: `Substring`, `Remove` and `Insert`":
//!   "Positions and lengths count code points (`Length`)."
//! cites: 9.6
//! expect: run
//! stdout: é
module Conf;

public class Program
  public static method Main()
    var s := "éa";
    Console.WriteLine(s.Remove(1, 1));
  end Main;
end Program;
