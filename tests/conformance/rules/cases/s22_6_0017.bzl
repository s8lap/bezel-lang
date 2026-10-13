//! case: s22_6_0017
//! doc: spec
//! section: 22.6
//! title: an owned/ordinary pair over a where-T-class constraint fails the declaration guard
//! rule: **`owned` never distinguishes overloads.** Two same-name members
//!   that differ only by `owned` on a parameter — an ordinary parameter or an
//!   `out` parameter — are indistinguishable and clash at the second
//!   declaration (§11.3): `Append(item: Widget)` beside `Append(owned item:
//!   Widget)` is a compile error
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
