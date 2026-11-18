//! case: s28_11_6902
//! doc: spec
//! section: 28.11
//! title: a local collection returned by move is not exempt, so its store is reported
//! rule: The trust-admission report (§28.3) names a parameter-, receiver- or
//!   field-path-sourced element at its store, and an element sourced in the body at a
//!   transfer of its source that the store reaches. A store into a collection that is a
//!   local of the body and does not escape it, of a parameter that is not `owned` or of
//!   `this` — the binding itself, not a value read through it — is not reported
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
  public method Make(item: Item): owned Array<Item>
    var tmp: Array<Item> := new Array<Item>();
    tmp.Add(item);
    return tmp;
  end Make;
end Prog;
