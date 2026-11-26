//! case: s28_3_0091
//! doc: spec
//! section: 28.3
//! title: under the trust-admission report mode, a scoped construction reports nothing
//! rule: Where the object that stores the borrow is the bound itself — a `using var`/`await using var`
//!   local `L` that the call constructs or initializes (forms 1/2), or `L` or `this` as the receiver with no
//!   `owned`-field hop (forms 4/5) — … the guarantee on the **source** is fully static, with no recorded
//!   trust clause (spec §28.14); the trust-admission report mode reports only admissions that rest on such
//!   a clause.
//! cites: 28.3, 28.14
//! expect: quiet
//! flags: --report-trust-admissions
//! issue: 4605
//! notes: The library has no call site, so no storing object is placed in an owned field; contrast
//!   s28_14_3602, whose placement carries the registration-lifetime residual.
module Conf;

public class Customer
  public constructor()
  end constructor;
end Customer;

public class Account
  private _owner: Customer;

  public constructor(scoped owner: Customer)
    _owner := owner;
  end constructor;
end Account;
