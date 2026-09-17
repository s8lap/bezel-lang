//! case: l6_9_0013
//! doc: stdlib
//! section: 6.9
//! title: disposing a pending Receive task deregisters without consuming an item
//! rule: Disposing a Receive task before an item has been dequeued into it
//!   deregisters the waiter; no message is consumed or lost — the next
//!   item goes to the next receive.
//! cites: 6.9, 30.15
//! expect: run
//! stdout: 1
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<Job>();
    using var snd := ch.NewSender();
    var pending := ch.Receive();
    pending.Dispose();
    snd.Send(new Job(1));
    using var a := await ch.Receive();
    Console.WriteLine(a.Id);
  end Main;
end Program;
