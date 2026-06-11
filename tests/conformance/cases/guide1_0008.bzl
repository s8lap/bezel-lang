method GetTransactions(): Array<Transaction>
  return _transactions;     // ERROR: cannot leak owned field through non-owned return
end GetTransactions;
