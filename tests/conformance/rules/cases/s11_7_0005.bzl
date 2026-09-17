//! case: s11_7_0005
//! doc: spec
//! section: 11.7
//! title: sealed abstract is rejected, the diagnostic naming closed
//! rule: Combining `closed` with `abstract`, `sealed`, or `static` is a
//!   compile error; the modifier pair `sealed abstract` is rejected too, the
//!   diagnostic naming `closed`.
//! cites: 11.7, C.1
//! expect: reject
//! diagnostic: closed
module Conf;

public sealed abstract class Shape
  public abstract method Area(): double;
end Shape;
