//! case: s17_13_0004
//! doc: spec
//! section: 17.13
//! title: out var _ is a compile error
//! rule: "`out var _` is likewise a compile error — it would declare
//!   nothing; write `out _`, which already means exactly this."
//! cites: 17.13, 22.4
//! expect: reject
//! diagnostic: out _
module Conf;

public class Program
  public static method Main()
    var dict: Dictionary<string, int> := ["a": 1];
    var found := dict.TryGetValue("a", out var _);
  end Main;
end Program;
