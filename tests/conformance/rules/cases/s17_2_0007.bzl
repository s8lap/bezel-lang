//! case: s17_2_0007
//! doc: spec
//! section: 17.2
//! title: bitwise OR mixed with logical or without parentheses is rejected
//! rule: Bitwise with logical (`and`, `or`) is non-associative across
//!   categories and requires explicit parenthesization when mixed.
//! cites: 17.2
//! expect: reject
//! diagnostic: parenthes
module Conf;

public class Ops
  public constructor()
  end constructor;

  public method Check(flags: int, mask: int, ready: bool): bool
    var r := flags | mask or ready;
    return r;
  end Check;
end Ops;
