//! case: s4_6_0008
//! doc: spec
//! section: 4.6
//! title: string ordering with non-nullable operands compiles
//! rule: Ordering requires non-nullable operands.
//! cites: 4.6
//! expect: accept
module Conf;

public class Holder
  public method Compute(a: string, b: string)
    if a < b then
    end if;
  end Compute;
end Holder;
