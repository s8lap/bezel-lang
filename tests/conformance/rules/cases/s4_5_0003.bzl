//! case: s4_5_0003
//! doc: spec
//! section: 4.5
//! title: a non-surrogate char literal compiles
//! rule: A single Unicode scalar value, 32 bits wide; surrogate code points
//!   are not valid char values, other code points are.
//! cites: 4.5
//! expect: accept
module Conf;

public class Holder
  public method Compute()
    var c: char := 'A';
  end Compute;
end Holder;
