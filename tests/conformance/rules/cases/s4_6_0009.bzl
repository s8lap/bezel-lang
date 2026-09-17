//! case: s4_6_0009
//! doc: spec
//! section: 4.6
//! title: the + operator performs no implicit ToString conversion
//! rule: The compiler does not implicitly convert other types to string; use
//!   interpolation or .ToString().
//! cites: 4.6
//! expect: reject
//! diagnostic: convert
module Conf;

public class Holder
  public method Compute(count: int)
    var s := "n = " + count;
  end Compute;
end Holder;
