//! case: s22_6_0017
//! doc: spec
//! section: 22.6
//! title: an owned/ordinary pair over a where-T-class constraint fails the declaration guard
//! rule: "**`where T: class` does not qualify** — it admits `string` and
//!   function-type substitutions, at which §28.1 erases the marker and the
//!   pair collapses to identical signatures. At any failing position the
//!   pair clashes: the declaration-site error ..., reported at the second
//!   declaration."
//! cites: 22.6, 28.1, 14.2
//! expect: reject
//! diagnostic: collapses
module Conf;

public class Box<T> where T: class
  public method Put(item: T)
  end Put;

  public method Put(owned item: T)
  end Put;
end Box;
