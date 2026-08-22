public class Account
  private _balance: decimal := 0m;
  private _limit: decimal := 0m;

  invariant(_balance + _limit >= 0m);

  public constructor(limit: decimal)
    requires(limit >= 0m)

    _limit := limit;
  end constructor;

  public method Withdraw(amount: decimal)
    requires(amount > 0m)
    requires(amount <= _balance + _limit)
    ensures(_balance = old(_balance) - amount)

    _balance := _balance - amount;
  end Withdraw;
end Account;
