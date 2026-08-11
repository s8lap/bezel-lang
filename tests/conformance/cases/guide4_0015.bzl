public record Transfer(From: string, To: string, Amount: decimal);   // sendable payload

public static async method LedgerMain(owned rx: Channel<Transfer>): Task
  using var inbox := move rx;                              // adopt the owned parameter
  using var balances := new Dictionary<string, decimal>(); // domain-local state: no lock
  await for var t in inbox.ReceiveAll() do                 // drain; suspends while empty
    balances[t.From] := (balances.GetOrNull(t.From) ?? 0m) - t.Amount;
    balances[t.To]   := (balances.GetOrNull(t.To)   ?? 0m) + t.Amount;
  end;                                                     // completes normally on close
end LedgerMain;

// Elsewhere:
var ledger := new Channel<Transfer>();
using var pay := ledger.NewSender();                       // mint before moving the channel
using var actor := Domain.Spawn(LedgerMain, move ledger);  // the receiving end moves in

pay.Send(new Transfer("ada", "lin", 25m));                 // value payload: copied across
pay.Dispose();                // last sender disposed: the channel closes and drains
await actor;                  // the actor finishes its inbox and exits; faults surface here
