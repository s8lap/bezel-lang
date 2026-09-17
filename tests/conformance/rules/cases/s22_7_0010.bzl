//! case: s22_7_0010
//! doc: spec
//! section: 22.7
//! title: a primary-constructor parameter list suppresses the implicit parameterless constructor
//! rule: Declaring **any** instance constructor suppresses the implicit
//!   parameterless constructor, a primary-constructor parameter list
//!   included.
//! cites: 22.7, 22.10
//! expect: reject
//! diagnostic: constructor
//! issue: 4364
module Conf;

public class Widget(size: int)
  public Size: int := size;
end Widget;

public class Program
  public method Run(): int
    using var w := new Widget();
    return w.Size;
  end Run;
end Program;
