//! case: s22_6_0018
//! doc: spec
//! section: 22.6
//! title: an owned/ordinary pair over a where-T-IDisposable constraint is rejected at the declaration (v0.338)
//! rule: **`owned` never distinguishes overloads (v0.338, proposal 0185).**
//!   An ordinary-`owned` / plain pair clashes at the second declaration; no
//!   constraint lifts it (§22.6, §11.3).
//! cites: 22.6, 28.1, 14.2
//! expect: reject
//! diagnostic: `owned` and `scoped` never distinguish overloads
module Conf;

public class Box<T> where T: IDisposable
  public method Put(item: T)
  end Put;

  public method Put(owned item: T)
  end Put;
end Box;
