//! case: s17_13_0014
//! doc: spec
//! section: 17.13
//! title: a bare _ in expression position is a compile error
//! rule: "a bare `_` in expression position (`_` is not a value — nothing
//!   binds it, so no such read can resolve)."
//! cites: 17.13
//! expect: reject
//! diagnostic: discard
module Conf;

public class Program
  public static method Main()
    var x := _;
  end Main;
end Program;
