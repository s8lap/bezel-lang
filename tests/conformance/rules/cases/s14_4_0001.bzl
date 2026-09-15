//! case: s14_4_0001
//! doc: spec
//! section: 14.4
//! title: owned does not propagate through a generic type argument
//! rule: `owned` does not propagate through generic type arguments — so
//!   `Array<owned T>` is not a type (§14.4).
//! cites: 14.4
//! expect: reject
//! diagnostic: owned
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private _items: Array<owned Widget>?;

  public constructor()
  end constructor;
end Holder;
