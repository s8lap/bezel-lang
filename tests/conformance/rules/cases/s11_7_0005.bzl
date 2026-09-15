//! case: s11_7_0005
//! doc: spec
//! section: 11.7
//! title: sealed abstract is rejected, the diagnostic naming closed
//! rule: Through v0.241 the semantics were spelled as the modifier pair
//!   sealed abstract; that spelling is rejected, the diagnostic naming
//!   closed.
//! cites: 11.7
//! expect: reject
//! diagnostic: closed
module Conf;

public sealed abstract class Shape
  public abstract method Area(): double;
end Shape;
