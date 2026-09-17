//! case: s4_3_0003
//! doc: spec
//! section: 4.3
//! title: a floating-point literal exceeding the type's largest finite value is a compile error
//! rule: a literal whose magnitude exceeds its type's largest finite value
//!   (1e999, or 1e40f against float) is a compile error.
//! cites: 4.3
//! expect: reject
//! diagnostic: magnitude
module Conf;

public class Holder
  public method Compute()
    var d := 1e999;
  end Compute;
end Holder;
