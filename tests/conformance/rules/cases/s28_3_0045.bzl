//! case: s28_3_0045
//! doc: spec
//! section: 28.3
//! title: a borrow of a binding atom's own owned binding may not escape the atom
//! rule: A borrow sourced by an atom binding may not escape the atom, not in the body's
//!   value; it is rejected, never lifetime-extended.
//! expect: reject
//! diagnostic: atom
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    var result := (var w := new Widget(); w);
  end Demo;
end Program;
