//! case: s28_3_0090
//! doc: spec
//! section: 28.3
//! title: under the report mode, a plain constructor parameter stored into a non-owned field reports
//! rule: In the trust-admission report mode, at every site where the compiler
//!   makes an admission that rests on a K4.3-recorded trust clause — a
//!   parameter-, receiver- or field-path-sourced borrow stored into a
//!   non-`owned` class- or interface-typed field — the compiler emits a
//!   warning naming the rule and its clause.
//! cites: 28.3, 28.5
//! expect: warn
//! diagnostic: trust
//! flags: --report-trust-admissions
module Conf;

public class Customer
  public constructor()
  end constructor;
end Customer;

public class Account
  private _owner: Customer;

  public constructor(owner: Customer)
    _owner := owner;
  end constructor;
end Account;
