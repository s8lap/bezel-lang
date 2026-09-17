//! case: s28_1_0008
//! doc: spec
//! section: 28.1
//! title: owned inline-array slot marker is admitted
//! rule: The `owned` keyword placed between the size and the closing bracket (`[N
//!   owned]T?`) marks the inline array's slots as owned.
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Basket
  private _slots: [3 owned]Widget?;

  public constructor()
    _slots := [null, null, null];
  end constructor;
end Basket;
