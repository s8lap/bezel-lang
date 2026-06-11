public property Balance: decimal
  get => _balance;
  set => do
    if value < 0m then throw new ArgumentException("Balance cannot be negative"); end;
    _balance := value;
  end;
end Balance;
