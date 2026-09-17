//! case: s11_3_0002
//! doc: spec
//! section: 11.3
//! title: two distinctly-named fields do not clash
//! rule: A type may not declare two members that are not distinguishable
//!   from each other -- distinctly-named members are unaffected.
//! cites: 11.3
//! expect: accept
module Conf;

public class Account
  private _balance: decimal := 0m;
  private _limit: decimal := 0m;

  public constructor()
  end constructor;
end Account;
