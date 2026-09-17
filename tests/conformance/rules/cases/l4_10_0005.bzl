//! case: l4_10_0005
//! doc: stdlib
//! section: 4.10
//! title: a Set<T> element type that is neither auto-hashable nor equality-declaring is rejected
//! rule: T must be hashable (spec §3.6/§8.3), enforced at the
//!   instantiation site per §3.6 rule 2 — same rule as Dictionary<K, V>
//!   keys.
//! cites: 4.10, 3.6, 8.3
//! expect: reject
//! diagnostic: hashable
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public record Key(Owner: Widget);

public class Program
  public method Demo()
    using var s := new Set<Key>();
  end Demo;
end Program;
