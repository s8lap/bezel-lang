//! case: s28_3_0091
//! doc: spec
//! section: 28.3
//! title: under the trust-admission report mode, a scoped construction reports nothing
//! rule: A class- or interface-typed `scoped` parameter upgrades the
//!   residual-trust back-reference admission to a static bound on the
//!   source, with no K4.3-recorded trust clause (spec §28.14); the
//!   trust-admission report mode reports only admissions that rest on such a
//!   clause.
//! cites: 28.3, 28.14
//! expect: quiet
//! flags: --report-trust-admissions
//! issue: 4605
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
