//! case: s28_5_0012
//! doc: spec
//! section: 28.5
//! title: an owning-inline-array field on a record is rejected
//! rule: Records also cannot have inline-array fields whose slots carry the
//!   `owned` marker (`[N owned]T?` or `[N owned]T`); such fields are move-only
//!   value types, and records support only copy semantics.
//! cites: 9.7, 28.5
//! expect: reject
//! diagnostic: move-only
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

record Bag
  private slots: [4 owned]Widget?;

  public constructor()
  end constructor;
end Bag;
