//! case: s4_6_0016
//! doc: spec
//! section: 4.6
//! title: a string has no Dispose() member
//! rule: Language spec §4.6 (v0.345): "A string has **no `Dispose()` member** — `s.Dispose()` is
//!   a compile error ([§8.7])".
//! cites: 4.6, 8.7
//! expect: reject
//! diagnostic: Dispose
module Conf;

public class Program
  public static method Main()
    var s := "abc";
    s.Dispose();
  end Main;
end Program;
