//! case: l4_8_0009
//! doc: stdlib
//! section: 4.8
//! title: a Dictionary<K,V> key type that is neither auto-hashable nor equality-declaring is rejected
//! rule: K must be hashable (spec §3.6/§8.3) — enforced at the
//!   instantiation site per §3.6 rule 2: a Dictionary<K, V> whose K is a
//!   record that is neither auto-hashable nor declares user equality is
//!   rejected with a diagnostic naming the property.
//! cites: 4.8, 3.6, 8.3
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
    using var d := new Dictionary<Key, int>();
  end Demo;
end Program;
