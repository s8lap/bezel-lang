//! case: s30_15_0010
//! doc: spec
//! section: 30.15
//! title: leaving a ReceiveAll loop early loses no item and releases the channel for TryReceive
//! rule: Abandoning the enumeration mid-drain deregisters like an abandoned
//!   `Receive` (below); no item is consumed or lost. … The
//!   at-most-one-consumption obligation releases when an enumeration ends by
//!   any edge — normal completion, fault, or enumerator disposal — after which
//!   a `Receive`, `TryReceive`, or fresh `ReceiveAll()` (a newly minted drain
//!   enumerable) is admissible.
//! cites: 30.15, 30.7
//! expect: run
//! output: 1
//!   True 2
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>();
    using var tx := ch.NewSender();
    tx.Send(1);
    tx.Send(2);
    tx.Send(3);
    await for var x in ch.ReceiveAll() do
      Console.WriteLine(x);
      break;
    end;
    var ok := ch.TryReceive(out var y);
    Console.WriteLine($"{ok} {y}");
  end Main;
end Program;
