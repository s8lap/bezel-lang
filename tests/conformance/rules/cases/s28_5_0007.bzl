//! case: s28_5_0007
//! doc: spec
//! section: 28.5
//! title: an inline-array field without the owned slot marker is admitted unmarked
//! rule: Inline-array fields (`[N]T`, `[N]T?`) without the `owned` slot marker
//!   may not be `owned`; declared without the field modifier, the field is
//!   ordinary borrow-holding storage.
//! cites: 28.1, 28.5
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Basket
  private _items: [4]Widget?;
end Basket;
