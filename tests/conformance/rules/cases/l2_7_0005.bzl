//! case: l2_7_0005
//! doc: stdlib
//! section: 2.7
//! title: Random is sendable and a moved instance continues its sequence
//! rule: `Random` is **sendable**, on the sendable form's copy-state ground.
//!   A moved instance continues its sequence in the receiving domain.
//! cites: 2.7, 1, 6.9
//! expect: run
//! stdout: continued
//! issue: 5587
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    using var inbox := new Channel<Random>();
    using var tx := inbox.NewSender();
    var r1 := new Random(42);
    _ := r1.Next();
    tx.Send(move r1);
    using var got := await inbox.Receive();
    using var r2 := new Random(42);
    _ := r2.Next();
    if got.Next() = r2.Next() then
      Console.WriteLine("continued");
    else
      Console.WriteLine("restarted");
    end;
  end Main;
end Program;
