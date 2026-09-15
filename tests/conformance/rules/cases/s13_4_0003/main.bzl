//! case: s13_4_0003
//! doc: spec
//! section: 13.4
//! title: files in Foo.Bar get no special access to files in Foo
//! rule: The dotted form is a single name; `Foo` and `Foo.Bar` are unrelated
//!   modules in both directions — neither gets special access to the other's
//!   internal members.
//! cites: 13.4, 13.1, 2.1
//! expect: reject
//! diagnostic: internal
module Foo.Bar;
import Foo;

public class Reader
  public constructor()
  end constructor;

  public method Peek(b: Box): int => b._value;
end Reader;
