//! case: s8_4_0003
//! doc: spec
//! section: 8.4
//! title: a class with no operator overload uses default Equals via =
//! rule: Operator overloading of `=` and `<>` is not permitted on classes;
//!   classes customize equality by overriding `Equals`.
//! cites: 8.4, C.10
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;
