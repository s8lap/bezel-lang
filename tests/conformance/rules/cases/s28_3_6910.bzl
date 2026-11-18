//! case: s28_3_6910
//! doc: spec
//! section: 28.3
//! title: a receiver-sourced owned-field borrow stored into a non-owned field draws a
//!   trust-admission line
//! rule: the trust-admission report (§28.3) names every such store, and a `scoped`
//!   parameter (§28.14) makes the bound static.
//! cites: 28.3
//! expect: warn
//! diagnostic: borrowed-field store
//! flags: --report-trust-admissions
//! issue: 7468
//! notes: The first sentence of the same bullet states the rule: a borrow may be stored in
//!   a field of an object only if the borrow's source provably outlives that object; clause
//!   3's residual is a K4.3-recorded trust clause, and the report mode names every such
//!   store.
module Conf;
public class Res
  public constructor() end constructor;
end Res;
public class Owner
  private owned _mine: Res;
  private _kept: Res? := null;
  public constructor()
    _mine := new Res();
  end constructor;
  public method Publish()
    _kept := this._mine;
  end Publish;
end Owner;
