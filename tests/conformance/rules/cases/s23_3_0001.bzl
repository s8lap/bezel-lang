//! case: s23_3_0001
//! doc: spec
//! section: 23.3
//! title: a block-form property names its setter parameter value implicitly
//! rule: The implicit setter parameter is `value`. Each accessor uses
//!   `=> expression` or `=> do ... end`.
//! cites: 23.3
//! expect: accept
module Conf;

public class Account
  private _balance: decimal := 0m;

  public constructor()
  end constructor;

  public property Balance: decimal
    get => _balance;
    set => do
      if value < 0m then
        throw new ArgumentException("Balance cannot be negative");
      end if;
      _balance := value;
    end;
  end Balance;
end Account;
