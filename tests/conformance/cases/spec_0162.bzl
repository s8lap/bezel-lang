public class Account
  private _balance: decimal := 0m;

  public property Balance: decimal
    pure get => _balance;
    set => _balance := value;
  end Balance;

  public pure property IsOverdrawn: bool => _balance < 0m;
end Account;
