//! case: s17_2_0010
//! doc: spec
//! section: 17.2
//! title: shift mixed with additive, parenthesized, is admitted
//! rule: "write `a << (2 + 1)` or `(a << 2) + 1`" — parenthesizing either
//!   subexpression disambiguates a shift-additive mix and is admitted.
//! cites: 17.2
//! expect: accept
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method Widen(a: int): int
    var r := (a << 2) + 1;
    return r;
  end Widen;
end Ops;
