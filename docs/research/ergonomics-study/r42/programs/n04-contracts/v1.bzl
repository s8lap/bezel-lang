module Bank.Accounts;

public class Account(owner: string)
  private _balance: decimal := 0m;
  public readonly property Owner: string := owner;
  public property Balance: decimal => _balance;

  invariant(_balance >= 0m);

  public static pure method Fee(amount: decimal): decimal => Decimal.Round(amount / 100m, 2);

  public method Deposit(amount: decimal)
    requires(amount > 0m)
    ensures(_balance = old(_balance) + amount)

    _balance := _balance + amount;
  end Deposit;

  public method Withdraw(amount: decimal): decimal
    requires(amount > 0m)
    requires(amount <= _balance)
    ensures(result = Fee(amount))

    _balance := _balance - amount;
    return Fee(amount);
  end Withdraw;
end Account;

public class Program
  public static method Main(): int
    using var acct := new Account("ann");
    acct.Deposit(100m);
    var fee := acct.Withdraw(30m);
    Console.WriteLine($"balance {acct.Balance}");
    Console.WriteLine($"fee {fee:F2}");
    try
      _ := acct.Withdraw(500m);
    catch
      ContractViolationException => Console.WriteLine("bug: contract violated")
    end;
    Console.WriteLine($"balance {acct.Balance}");
    return 0;
  end Main;
end Program;
