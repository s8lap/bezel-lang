//! case: s17_2_0002
//! doc: spec
//! section: 17.2
//! title: bitwise AND mixed with equality, parenthesized, is admitted
//! rule: "write `(a & 1) = 0`" — the mandatory-parenthesization rule applies
//!   only across the listed categories; parenthesizing the bitwise operand
//!   disambiguates and is admitted.
//! cites: 17.2
//! expect: accept
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method IsEven(a: int): bool
    var r := (a & 1) = 0;
    return r;
  end IsEven;
end Ops;
