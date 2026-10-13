//! case: s28_3_0090
//! doc: spec
//! section: 28.3
//! title: under the report mode, a plain constructor parameter stored into a non-owned field reports
//! rule: In that mode, at every site where the compiler makes an admission
//!   that rests on a K4.3-recorded trust clause, it **emits a warning** ...
//!   the **field-held** clause (the borrowed-field store rule above, wherever
//!   restated) ... **The warning** names the rule and its clause, the borrow
//!   and its source, the holder ... The site is the admitting site — the
//!   store, the call, the abandon or the `await` the clause admits
//! cites: 28.3, 28.5
//! expect: warn
//! diagnostic: trust
//! flags: --report-trust-admissions
//! issue: 4605
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
