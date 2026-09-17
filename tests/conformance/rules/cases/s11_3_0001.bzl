//! case: s11_3_0001
//! doc: spec
//! section: 11.3
//! title: two fields with the same name clash
//! rule: A type may not declare two members that are not distinguishable
//!   from each other. Two members clash when they have the same name...
//!   two fields, properties, or nested types with the same name.
//! cites: 11.3
//! expect: reject
//! diagnostic: clash
module Conf;

public class Account
  private _balance: decimal := 0m;
  private _balance: decimal := 0m;

  public constructor()
  end constructor;
end Account;
