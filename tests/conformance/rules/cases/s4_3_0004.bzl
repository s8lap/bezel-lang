//! case: s4_3_0004
//! doc: spec
//! section: 4.3
//! title: a floating-point literal within range compiles
//! rule: A floating-point literal denotes the nearest representable value of
//!   its type: its value rounded as IEEE 754 rounds to nearest, a tie to the
//!   even significand. A literal whose value, so rounded, is not finite is a
//!   compile error.
//! cites: 4.3
//! expect: accept
module Conf;

public class Holder
  public method Compute()
    var d := 1e10;
  end Compute;
end Holder;
