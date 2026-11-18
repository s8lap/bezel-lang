//! case: s28_11_6906
//! doc: spec
//! section: 28.11
//! title: a store through a local alias of a field-held dictionary is reported
//! rule: The trust-admission report (§28.3) names a parameter-, receiver- or
//!   field-path-sourced element at its store, and an element sourced in the body at a
//!   transfer of its source that the store reaches.
//! cites: 28.11, 28.3
//! expect: warn
//! diagnostic: borrowing-collection store
//! flags: --report-trust-admissions
//! issue: 7468
//! notes: The exemption is conditioned on the collection not escaping the body and on the
//!   store naming the binding itself; the stores below name the plain parameter's borrow,
//!   which is reported at its store.
module Conf;
public class Item
  public constructor() end constructor;
end Item;
public class Prog
  private owned _d: Dictionary<int, Item> := new Dictionary<int, Item>();
  public method Take(item: Item)
    var tmp := _d;
    tmp.Add(1, item);
  end Take;
end Prog;
