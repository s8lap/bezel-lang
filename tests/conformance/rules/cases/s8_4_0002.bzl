//! case: s8_4_0002
//! doc: spec
//! section: 8.4
//! title: operator overloading of = is not permitted on classes
//! rule: Operator overloading of `=` and `<>` is not permitted on classes;
//!   classes customize equality by overriding `Equals`.
//! cites: 8.4, C.10
//! expect: reject
//! diagnostic: not permitted on classes
module Conf;

public class Widget
  public constructor()
  end constructor;

  public static method operator =(a: Widget, b: Widget): bool => true;
end Widget;
