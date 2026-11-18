//! case: s28_3_7456
//! doc: spec
//! section: 28.3
//! title: a non-static field read through a static holder stored into a non-owned field draws a trust
//!   line
//! rule: the trust-admission report ([§28.3](#sec-diagnostics)) names every such store, and a
//!   `scoped` parameter ([§28.14](#sec-scoped-callback-parameters)) makes the bound static
//! cites: 28.3, 28.11
//! expect: warn
//! diagnostic: borrowed-field store
//! flags: --report-trust-admissions
//! notes: Review r42-rev-m32d-7468 (#7468): a non-static member reached through a static holder is
//!   still a borrow whose source the caller must keep alive; only the static storage itself is
//!   always-live (producers table, static field access). The report names the store.
module Conf;
public class Item
  public constructor() end constructor;
end Item;
public class Holder
  public Child: Item?;
end Holder;
public class Reg
  public static readonly H: Holder := new Holder();
end Reg;
public class Prog
  private _kept: Item?;
  public method Take()
    _kept := Reg.H.Child;
  end Take;
end Prog;
