//! case: s28_3_6913
//! doc: spec
//! section: 28.3
//! title: a DictionaryEntry Value read stored into a non-owned field draws a
//!   trust-admission line
//! rule: the trust-admission report (§28.3) names every such store, and a `scoped`
//!   parameter (§28.14) makes the bound static.
//! cites: 28.3, 28.11
//! expect: warn
//! diagnostic: trust admission
//! flags: --report-trust-admissions
//! issue: 7468
//! notes: The first sentence of the same bullet states the rule: a borrow may be stored in
//!   a field of an object only if the borrow's source provably outlives that object; clause
//!   3's residual is a K4.3-recorded trust clause, and the report mode names every such
//!   store.
module Conf;
public class Item
  public constructor() end constructor;
end Item;
public class Prog
  private _kept: Item?;
  public method Take(e: DictionaryEntry<string, Item>)
    _kept := e.Value;
  end Take;
end Prog;
