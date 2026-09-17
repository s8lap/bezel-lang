//! case: s17_15_0008
//! doc: spec
//! section: 17.15
//! title: the binding atom is admitted inside a contract clause
//! rule: "In contract clauses (§22.16) the atom is admitted — it is the
//!   form's motivating position: clauses are expression-only and pure, so
//!   without the atom a clause using a subresult twice would have to
//!   recompute it."
//! cites: 17.15, 22.16
//! expect: accept
module Conf;

public class Account
  private _balance: decimal;

  public constructor(balance: decimal)
    _balance := balance;
  end constructor;

  public method Withdraw(amount: decimal)
    requires((var ok := amount > 0m and amount <= _balance; ok))

    _balance := _balance - amount;
  end Withdraw;
end Account;
