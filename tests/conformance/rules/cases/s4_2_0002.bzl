//! case: s4_2_0002
//! doc: spec
//! section: 4.2
//! title: an out-of-range unsuffixed integer literal in an untyped context is a compile error
//! rule: In an untyped context, an unsuffixed integer literal exceeding int's
//!   range is a compile error directing to a suffix.
//! cites: 4.2
//! expect: reject
//! diagnostic: suffix
module Conf;

public class Holder
  public method Compute()
    var x := 3_000_000_000;
  end Compute;
end Holder;
