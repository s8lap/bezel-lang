//! case: s17_13_0001
//! doc: spec
//! section: 17.13
//! title: out _ discards the out-parameter result
//! rule: "As a positional argument with the `out` modifier, `_` discards
//!   the out-parameter result ... `if dict.TryGetValue(key, out _) then ...`"
//! cites: 17.13
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var dict: Dictionary<string, int> := ["a": 1];
    var found := dict.TryGetValue("a", out _);
    Console.WriteLine(found);
  end Main;
end Program;
