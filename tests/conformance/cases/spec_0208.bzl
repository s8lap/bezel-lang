public method GetTransactions(): OwningArray<Transaction>
  return _transactions;     // OK: borrows the field — the caller may
                            //   read/iterate/mutate/drain it but not dispose it.
end GetTransactions;

public method TakeTransactions(): owned OwningArray<Transaction>
  var fresh := new OwningArray<Transaction>();  // build the replacement first (§28.8.1)
  var taken := move _transactions;              // take: the field is now moved-from
  _transactions := fresh;                       // restore — required on every exit path (§28.8.1)
  return taken;                                 // transfer: the caller owns the taken array
end TakeTransactions;

public method LeakTransactions(): OwningArray<Transaction>
  return new OwningArray<Transaction>();  // ERROR: a freshly-produced owning
                                          //   value through a non-`owned`
                                          //   return escapes with no owner.
end LeakTransactions;
