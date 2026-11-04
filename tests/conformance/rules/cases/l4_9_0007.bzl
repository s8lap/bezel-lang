//! case: l4_9_0007
//! doc: stdlib
//! section: 4.9
//! title: a fresh class-typed key into an OwningDictionary is rejected
//! rule: A fresh unbound owned temporary of class type ... in ... the **key** position
//!   of `OwningDictionary<K, V>`, which owns its values and not its keys (stdlib
//!   §4.9), so a class-typed key is held as a borrow exactly as
//!   `Dictionary<K, V>`'s is, is a compile error at the store site (§15.4,
//!   v0.355).
//! cites: 4.9, 15.4, 28.11
//! expect: reject
//! diagnostic: §15.4 fresh-store error
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Key
  public constructor()
  end constructor;
end Key;

public class Program
  public method Fill()
    using var d := new OwningDictionary<Key, Widget>();
    d[new Key()] := new Widget();
  end Fill;
end Program;
