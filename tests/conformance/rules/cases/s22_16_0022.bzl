//! case: s22_16_0022
//! doc: spec
//! section: 22.16
//! title: an invariant member with no visibility keyword is admitted
//! rule: `invariant(E);` carries no visibility keyword — for such a
//!   member no visibility choice exists to spell.
//! cites: 22.16, 13.3, C.2
//! expect: accept
module Conf;

public class Account
  private _balance: decimal := 0m;

  invariant(_balance >= 0m);

  public constructor()
  end constructor;
end Account;
