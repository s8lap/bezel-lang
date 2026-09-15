//! case: s22_16_0021
//! doc: spec
//! section: 22.16
//! title: an invariant member given a visibility keyword is rejected
//! rule: `invariant(E);` declares no visibility choice to spell; it
//!   carries no visibility keyword (§13.3's principle).
//! cites: 22.16, 13.3, C.2
//! expect: reject
//! diagnostic: visibility
module Conf;

public class Account
  private _balance: decimal := 0m;

  public invariant(_balance >= 0m);

  public constructor()
  end constructor;
end Account;
