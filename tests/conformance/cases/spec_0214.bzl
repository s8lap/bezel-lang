// Migrated for v0.213 (§28.11, axiom A8 retired, #2081): the holder that owns
// its elements declares the OWNING family; `owned` on the field disposes the
// container and the owning collection's cascade disposes every element — no
// destructor and no `DisposeElements()` pass (both retired with A8).
class BankAccount
  private owned _transactions: OwningArray<Transaction>;

  public constructor()
    _transactions := new OwningArray<Transaction>();
  end constructor;
end BankAccount;
