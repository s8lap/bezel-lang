//! case: s11_7_0007
//! doc: spec
//! section: 11.7
//! title: closed combined with abstract is rejected
//! rule: Combining closed with abstract, sealed, or static is a compile
//!   error.
//! cites: 11.7
//! expect: reject
//! diagnostic: closed
module Conf;

public closed abstract class Shape
  public abstract method Area(): double;
end Shape;
