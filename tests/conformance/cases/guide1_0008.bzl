method GetTransactions(): Array<Transaction>
  return _transactions;       // OK: borrows the field — the caller may read
                              //   it but not dispose or store it as owned.
end GetTransactions;

method TakeTransactions(): owned Array<Transaction>
  var fresh := new Array<Transaction>();  // build the replacement first (next section)
  var taken := move _transactions;        // take: the field is now moved-from
  _transactions := fresh;                 // restore — every exit path must (spec §28.8.1)
  return taken;                           // transfer: hand the caller ownership
end TakeTransactions;
