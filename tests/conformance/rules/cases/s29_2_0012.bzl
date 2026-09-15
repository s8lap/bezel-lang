//! case: s29_2_0012
//! doc: spec
//! section: 29.2
//! title: OwningDictionary has no default entry iteration
//! rule: `for` operates only on a value whose type implements `IEnumerable<T>`;
//!   `OwningDictionary<K, V>`'s interface conformance is `IDisposable` only, so
//!   default `for var entry in ownedDict do` iteration is not provided for it
//!   (stdlib §4.9).
//! cites: 29.1, 29.2
//! expect: reject
//! diagnostic: does not implement
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Program
  public static method Main()
    using var od := new OwningDictionary<string, Widget>();
    od["a"] := new Widget();
    for var entry in od do
      _ := entry;
    end;
  end Main;
end Program;
