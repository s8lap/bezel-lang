//! case: s28_3_0093
//! doc: spec
//! section: 28.3
//! title: under the report mode, a parameter borrow added to a borrowing collection field reports
//! rule: In the trust-admission report mode, a parameter-, receiver- or
//!   field-path-sourced borrow stored into a borrowing-family collection
//!   (spec §28.11's store rule) is an admission resting on a K4.3-recorded
//!   trust clause, and the compiler emits a warning naming the rule and its
//!   clause.
//! cites: 28.3, 28.11
//! expect: warn
//! diagnostic: trust
//! flags: --report-trust-admissions
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public class Index
  private owned _items: Array<Item>;

  public constructor()
    _items := new Array<Item>();
  end constructor;

  public method Register(item: Item)
    _items.Add(item);
  end Register;
end Index;
