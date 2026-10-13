//! case: s11_3_0007
//! doc: spec
//! section: 11.3
//! title: an owned/plain parameter pair over a where-T-IDisposable constraint is rejected (v0.338)
//! rule: **`owned` never distinguishes overloads (v0.338, proposal 0185).**
//!   An ordinary-`owned` / plain pair clashes at the second declaration (§11.3).
//! cites: 11.3, 22.6
//! expect: reject
//! diagnostic: `owned` and `scoped` never distinguish overloads
module Conf;

public class Registry<T> where T: IDisposable
  public constructor()
  end constructor;

  public method Store(item: T)
  end Store;

  public method Store(owned item: T)
  end Store;
end Registry;
