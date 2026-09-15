//! case: s17_13_0013
//! doc: spec
//! section: 17.13
//! title: a parameter named _ is a compile error
//! rule: "every declaration named `_` — a method or constructor parameter
//!   (parameter names are call-site-addressable API surface, carried by the
//!   artifact interface — choose a name) ... is a compile error whose
//!   diagnostic names the sanctioned forms."
//! cites: 17.13, 22.5
//! expect: reject
//! diagnostic: discard
module Conf;

public class Program
  public static method Add(_: int, y: int): int => _ + y;
end Program;
