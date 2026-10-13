//! case: s28_3_0093
//! doc: spec
//! section: 28.3
//! title: under the report mode, a parameter borrow added to a borrowing collection field reports
//! rule: In that mode, at every site where the compiler makes an admission
//!   that rests on a K4.3-recorded trust clause, it **emits a warning** ...
//!   the **collection-held** clause (§28.11's store rule, a store whose
//!   receiver the structural check leaves unclassified included) ... **The
//!   warning** names the rule and its clause ... The trust-admission report
//!   (§28.3) names a parameter-, receiver- or field-path-sourced element at
//!   its store
//! cites: 28.3, 28.11
//! expect: warn
//! diagnostic: trust
//! flags: --report-trust-admissions
//! issue: 4605
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
