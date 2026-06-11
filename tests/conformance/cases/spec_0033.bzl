interface IBankAccount
  method Deposit(amount: decimal);
  method Withdraw(amount: decimal);
  property Balance: decimal;
  readonly property AccountNumber: string;
  event BalanceChanged: BalanceChangedEvent;
end IBankAccount;
