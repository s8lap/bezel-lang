//! case: s4_6_0007
//! doc: spec
//! section: 4.6
//! title: string ordering requires non-nullable operands
//! rule: Ordering requires non-nullable operands: an operand of static type
//!   string? is a compile error.
//! cites: 4.6, 7.11
//! expect: reject
//! diagnostic: non-nullable
module Conf;

public class Holder
  public method Compute(a: string?, b: string?)
    if a < b then
    end if;
  end Compute;
end Holder;
