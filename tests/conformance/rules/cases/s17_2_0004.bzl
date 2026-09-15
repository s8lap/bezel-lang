//! case: s17_2_0004
//! doc: spec
//! section: 17.2
//! title: bitwise AND mixed with a relational operator, parenthesized, is admitted
//! rule: Parenthesizing the bitwise operand disambiguates a bitwise-relational
//!   mix and is admitted (§17.2's mandatory-parenthesization rule).
//! cites: 17.2
//! expect: accept
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method Check(a: int): bool
    var r := (a & 1) < 2;
    return r;
  end Check;
end Ops;
