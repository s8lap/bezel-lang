//! case: s4_2_0004
//! doc: spec
//! section: 4.2
//! title: a suffixed literal exceeding its suffix's range is a compile error
//! rule: A suffixed integer literal whose value does not fit the suffix's
//!   type is a compile error.
//! cites: 4.2
//! expect: reject
//! diagnostic: fit
module Conf;

public class Holder
  public method Compute()
    var x := 5000000000U;
  end Compute;
end Holder;
