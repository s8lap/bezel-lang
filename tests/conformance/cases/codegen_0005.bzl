public method Withdraw(amount: decimal)
  requires(amount > 0m)
  requires(amount <= _balance)
  ensures(_balance = old(_balance) - amount)

  _balance := _balance - amount;
end Withdraw;
