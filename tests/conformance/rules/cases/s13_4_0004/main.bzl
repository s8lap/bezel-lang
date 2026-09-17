//! case: s13_4_0004
//! doc: spec
//! section: 13.4
//! title: two modules under a common dotted prefix are unrelated siblings
//! rule: The dotted form is a single name; it does not establish nesting
//!   (§2.1). `Foo.Bar` and `Foo.Baz` share a prefix but are unrelated
//!   modules — neither gets access to the other's internal members.
//! cites: 13.4, 13.1, 2.1
//! expect: reject
//! diagnostic: internal
module Foo.Bar;
import Foo.Baz;

public class Reader
  public constructor()
  end constructor;

  public method Peek(b: Box): int => b._value;
end Reader;
