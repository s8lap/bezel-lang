//! case: s29_2_0012
//! doc: spec
//! section: 29.2
//! title: OwningDictionary has no default entry iteration
//! rule: `OwningDictionary<K,V>` has no default entry iteration — its
//!   conformance is `IDisposable` only.
//! cites: 29.2
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
