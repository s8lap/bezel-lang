// REJECT test — `.DisposeElements()` is retired with axiom A8 (v0.213, #2081).
//
// Spec §28.11 (v0.213) retires axiom A8 (the transit exception) and, with it,
// the bulk `DisposeElements()` escape hatch: the `DisposeElements` declarations
// were removed from `ICollection<T>`, `Array<T>`, `LinkedList<T>`, and
// `Dictionary<K, V>` (see the v0.213 changelog). A borrowing collection is a
// transit point and never disposes its elements; a holder that owns its elements
// declares the OWNING family (whose `Dispose()` cascades), or takes each element
// out first (`Drain()`, the `Pop*` family, `move coll[i]`) into an owned binding.
//
// Here `_transactions` is a borrowing `Array<Transaction>`, so the destructor's
// `_transactions.DisposeElements()` — the canonical pre-v0.213 shape (this is the
// spec's own former BankAccount example) — is now rejected: `DisposeElements` is
// no longer on the collection surface. The migrated, accepting form declares
// `OwningArray<Transaction>` and drops the destructor (the cascade is automatic);
// see tests/conformance/cases/spec_0214.bzl.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
module M;
class Transaction public method Ping(): () end Ping; end Transaction;
class BankAccount
  private owned _transactions: Array<Transaction>;
  public constructor()
    _transactions := new Array<Transaction>();
  end constructor;
  destructor
    _transactions.DisposeElements();
  end destructor;
end BankAccount;
class Program
  public static method Main(): int return 0; end Main;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: `.DisposeElements()` was removed from the collection surface
// CHECK-SAME: v0.213
// CHECK-SAME: axiom A8
