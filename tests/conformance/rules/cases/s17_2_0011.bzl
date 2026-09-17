//! case: s17_2_0011
//! doc: spec
//! section: 17.2
//! title: range mixed with bitwise AND without parentheses is rejected
//! rule: Range (`..`, `..<`) with bitwise (`&`, `|`, `^`) is non-associative
//!   across categories; `mask & 0..n` is a compile error.
//! cites: 17.2
//! expect: reject
//! diagnostic: parenthes
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method Make(mask: int, n: int)
    var r := mask & 0..n;
  end Make;
end Ops;
