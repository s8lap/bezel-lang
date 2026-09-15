//! case: s17_2_0006
//! doc: spec
//! section: 17.2
//! title: bitwise AND mixed with logical and, parenthesized, is admitted
//! rule: "write `(flags & MASK) <> 0 and ready`" — parenthesizing the bitwise
//!   subexpression, and comparing it to `0` to yield a `bool`, disambiguates
//!   and is admitted.
//! cites: 17.2
//! expect: accept
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method Check(flags: int, mask: int, ready: bool): bool
    var r := (flags & mask) <> 0 and ready;
    return r;
  end Check;
end Ops;
