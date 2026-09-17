//! case: l4_9_0007
//! doc: stdlib
//! section: 4.9
//! title: a fresh class-typed key into an OwningDictionary warns
//! rule: A class-typed key is held as a borrow exactly as Dictionary<K,V>'s
//!   is, and the fresh-store warning reaches that position: od[key] := v
//!   with a fresh key temporary dangles at the K8 boundary.
//! cites: 4.9, 15.4, 28.11
//! expect: warn
//! diagnostic: dangl
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
