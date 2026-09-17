//! case: s17_2_0009
//! doc: spec
//! section: 17.2
//! title: shift mixed with additive without parentheses is rejected
//! rule: Shift (`<<`, `>>`) with arithmetic (`+`, `-`) is non-associative
//!   across categories; `a << 2 + 1` is a compile error.
//! cites: 17.2
//! expect: reject
//! diagnostic: parenthes
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method Widen(a: int): int
    var r := a << 2 + 1;
    return r;
  end Widen;
end Ops;
