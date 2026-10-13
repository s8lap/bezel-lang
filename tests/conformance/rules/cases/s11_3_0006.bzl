//! case: s11_3_0006
//! doc: spec
//! section: 11.3
//! title: an owned/plain parameter pair fails the declaration guard when it can collapse
//! rule: **`owned` never distinguishes overloads.** Two same-name members
//!   that differ only by `owned` on a parameter — an ordinary parameter or an
//!   `out` parameter — are indistinguishable and clash at the second
//!   declaration (§11.3): `Append(item: Widget)` beside `Append(owned item:
//!   Widget)` is a compile error
//! cites: 11.3, 22.6
//! expect: reject
//! diagnostic: clash
module Conf;

public class Registry<T> where T: class
  public constructor()
  end constructor;

  public method Store(item: T)
  end Store;

  public method Store(owned item: T)
  end Store;
end Registry;
