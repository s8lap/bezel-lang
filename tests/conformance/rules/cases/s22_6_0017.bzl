//! case: s22_6_0017
//! doc: spec
//! section: 22.6
//! title: an owned/ordinary pair over a where-T-class constraint clashes at the declaration (v0.338)
//! rule: **`owned` never distinguishes overloads (v0.338, proposal 0185).**
//!   Two same-name members that differ only by ordinary `owned` clash at the
//!   second declaration (§22.6, §11.3): `Append(item: Widget)` beside
//!   `Append(owned item: Widget)` is a compile error.
//! cites: 22.6, 28.1, 14.2
//! expect: reject
//! diagnostic: `owned` and `scoped` never distinguish overloads
module Conf;

public class Box<T> where T: class
  public method Put(item: T)
  end Put;

  public method Put(owned item: T)
  end Put;
end Box;
