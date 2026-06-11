/// <summary>
/// Represents a bank account with deposit and withdrawal functionality.
/// </summary>
public class BankAccount: IBankAccount
  /// <summary>
  /// Deposits the specified amount into the account.
  /// </summary>
  /// <param name="amount">The amount to deposit. Must be positive.</param>
  /// <exception cref="ArgumentException">Thrown when amount is non-positive.</exception>
  public method Deposit(amount: decimal)
    if amount <= 0m then
      throw new ArgumentException("Amount must be positive.", nameof(amount));
    end;
    Balance := Balance + amount;
  end Deposit;
end BankAccount;
