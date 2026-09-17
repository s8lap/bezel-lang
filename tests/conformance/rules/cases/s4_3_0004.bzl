//! case: s4_3_0004
//! doc: spec
//! section: 4.3
//! title: a floating-point literal within range compiles
//! rule: A floating-point literal denotes the nearest representable value of
//!   its type, provided its magnitude does not exceed the type's largest
//!   finite value.
//! cites: 4.3
//! expect: accept
module Conf;

public class Holder
  public method Compute()
    var d := 1e10;
  end Compute;
end Holder;
