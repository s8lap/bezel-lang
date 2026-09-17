//! case: s2_2_0001
//! doc: spec
//! section: 2.2
//! title: an import alias is usable as a type qualifier
//! rule: An import may bind a file-scoped alias: `import BankingApp.Transactions
//!   as Tx;` — thereafter `Tx.Ledger` names that module's public `Ledger`.
//! expect: accept
module Conf;
import System.Threading as Th;

public class Holder
  private method Use()
    using var cts := new Th.CancellationTokenSource();
  end Use;
end Holder;
