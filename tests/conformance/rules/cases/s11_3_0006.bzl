//! case: s11_3_0006
//! doc: spec
//! section: 11.3
//! title: an owned/plain parameter pair over a where-T-class constraint clashes at the declaration (v0.338)
//! rule: **`owned` never distinguishes overloads (v0.338, proposal 0185).**
//!   Two same-name members that differ only by ordinary `owned` clash at the
//!   second declaration (§22.6, §11.3): `Append(item: Widget)` beside
//!   `Append(owned item: Widget)` is a compile error.
//! cites: 11.3, 22.6
//! expect: reject
//! diagnostic: `owned` and `scoped` never distinguish overloads
module Conf;

public class Registry<T> where T: class
  public constructor()
  end constructor;

  public method Store(item: T)
  end Store;

  public method Store(owned item: T)
  end Store;
end Registry;
