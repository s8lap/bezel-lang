//! case: s17_7_0009
//! doc: spec
//! section: 17.7
//! title: the new() shorthand is not admitted at a typed assignment target
//! rule: "nor at a typed assignment target (`xs := new();` — §14.5's target
//!   contexts include assignments; this shorthand does not)."
//! cites: 17.7
//! expect: reject
//! diagnostic: shorthand
module Conf;

public class Program
  public static method Main()
    var xs: Array<int> := new Array<int>();
    xs := new();
  end Main;
end Program;
