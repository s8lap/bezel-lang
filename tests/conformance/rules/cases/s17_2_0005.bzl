//! case: s17_2_0005
//! doc: spec
//! section: 17.2
//! title: bitwise AND mixed with logical and without parentheses is rejected
//! rule: Bitwise with logical (`and`, `or`) is non-associative across
//!   categories; `flags & MASK and ready` is a compile error.
//! cites: 17.2
//! expect: reject
//! diagnostic: parenthes
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method Check(flags: int, mask: int, ready: bool): bool
    var r := flags & mask and ready;
    return r;
  end Check;
end Ops;
