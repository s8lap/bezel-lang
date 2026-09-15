//! case: l4_9_0008
//! doc: stdlib
//! section: 4.9
//! title: OwningDictionary<K,V>.GetOrAdd returns a borrow of the stored value
//! rule: GetOrAdd(key: K, factory: (K) -> owned V): V; // returns borrow —
//!   the dictionary stores the factory's value, taking ownership, and the
//!   call returns a borrow of the now-stored value.
//! cites: 4.9, 28.11
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public method Demo()
    using var d := new OwningDictionary<string, Widget>();
    var w := d.GetOrAdd("a", lambda(k: string) => new Widget());
    var again := d.GetOrAdd("a", lambda(k: string) => new Widget());
  end Demo;
end Program;
