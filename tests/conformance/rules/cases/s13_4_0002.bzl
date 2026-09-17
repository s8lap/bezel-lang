//! case: s13_4_0002
//! doc: spec
//! section: 13.4
//! title: a dotted module name is an ordinary whole name for internal access
//! rule: A module is the set of all source files declaring the same
//!   `module Name.Path;`; the dotted form is a single name, so files sharing
//!   the whole dotted name share internal access exactly as an undotted name
//!   would.
//! cites: 13.4
//! expect: accept
module Foo.Bar;

public class Box
  internal _value: int;

  public constructor(value: int)
    _value := value;
  end constructor;
end Box;

public class Reader
  public constructor()
  end constructor;

  public method Peek(b: Box): int => b._value;
end Reader;
