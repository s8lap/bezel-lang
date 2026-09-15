//! case: s17_2_0008
//! doc: spec
//! section: 17.2
//! title: bitwise OR mixed with logical or, parenthesized, is admitted
//! rule: Parenthesizing the bitwise subexpression and comparing it to `0`
//!   to yield a `bool` disambiguates a bitwise-logical mix and is admitted
//!   (§17.2's mandatory-parenthesization rule).
//! cites: 17.2
//! expect: accept
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method Check(flags: int, mask: int, ready: bool): bool
    var r := (flags | mask) <> 0 or ready;
    return r;
  end Check;
end Ops;
