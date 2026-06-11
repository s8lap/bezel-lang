public method GetTransactions(): Array<Transaction>
  return _transactions;          // ERROR: cannot leak owned field
end GetTransactions;
