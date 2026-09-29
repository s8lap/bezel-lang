//! case: s4_3_0003
//! doc: spec
//! section: 4.3
//! title: a floating-point literal whose IEEE rounding is not finite is a compile error
//! rule: A literal whose value, so rounded (as IEEE 754 rounds to nearest, a
//!   tie to the even significand), is not finite is a compile error: its
//!   magnitude is at least its type's largest finite value plus half a unit in
//!   that value's last place — 1e999 and 1e40f are out of range.
//! cites: 4.3
//! expect: reject
//! diagnostic: magnitude
module Conf;

public class Holder
  public method Compute()
    var d := 1e999;
  end Compute;
end Holder;
