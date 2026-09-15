//! case: s14_2_0014
//! doc: spec
//! section: 14.2
//! title: contradictory constraints are a compile error at the declaration
//! rule: Contradictory constraint combinations (e.g. `class, value`) are
//!   compile errors (§14.2).
//! cites: 14.2
//! expect: reject
//! diagnostic: contradictory
module Conf;

public class Box<T> where T: class, value
  public constructor()
  end constructor;
end Box;
