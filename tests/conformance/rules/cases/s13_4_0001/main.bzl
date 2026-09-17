//! case: s13_4_0001
//! doc: spec
//! section: 13.4
//! title: files in Foo get no special access to files in Foo.Bar
//! rule: The dotted form is a single name; files in `Foo` get no special
//!   access to files in `Foo.Bar`.
//! cites: 13.4, 13.1, 2.1
//! expect: reject
//! diagnostic: internal
module Foo;
import Foo.Bar;

public class Reader
  public constructor()
  end constructor;

  public method Peek(b: Box): int => b._value;
end Reader;
