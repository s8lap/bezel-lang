//! case: s28_5_0006
//! doc: spec
//! section: 28.5
//! title: owned on an inline-array field without the owned slot marker is rejected
//! rule: Inline-array fields (`[N]T`, `[N]T?`) without the `owned` slot marker
//!   may not be `owned`. The array's slots hold borrows; there is no per-slot
//!   ownership to manage.
//! cites: 28.1, 28.5
//! expect: reject
//! diagnostic: slot marker
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Basket
  private owned _items: [4]Widget?;
end Basket;
