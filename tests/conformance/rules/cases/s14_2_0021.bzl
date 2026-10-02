//! case: s14_2_0021
//! doc: spec
//! section: 14.2
//! title: a nullable class type element is rejected on OwningArray<T>
//! rule: `where T: class` admits only non-nullable reference types; a
//!   nullable element wrapper is rejected at the instantiation site (§14.2,
//!   v0.307, #4796).
//! cites: 14.2
//! expect: reject
//! diagnostic: non-nullable
//! issue: 4796
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private _p: OwningArray<Widget?>?;

  public constructor()
  end constructor;
end Holder;