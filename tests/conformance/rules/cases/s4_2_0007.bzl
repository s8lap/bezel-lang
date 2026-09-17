//! case: s4_2_0007
//! doc: spec
//! section: 4.2
//! title: the same magnitude without the immediate unary-minus position overflows int's range
//! rule: Anywhere else those magnitudes remain errors — the magnitude alone
//!   overflows before the - applies.
//! cites: 4.2
//! expect: reject
//! diagnostic: magnitude
module Conf;

public class Holder
  public method Compute()
    var x: int := 2147483648;
  end Compute;
end Holder;
