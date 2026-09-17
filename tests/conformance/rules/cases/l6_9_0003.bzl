//! case: l6_9_0003
//! doc: stdlib
//! section: 6.9
//! title: items from one sender are received in per-sender FIFO order
//! rule: Ordering. Per-sender FIFO; cross-sender interleaving is an
//!   unspecified consistent total order; each item is received exactly
//!   once.
//! cites: 6.9, 30.15
//! expect: run
//! output: 1
//!   2
//!   3
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
    snd.Send(new Job(1));
    snd.Send(new Job(2));
    snd.Send(new Job(3));
    using var a := await ch.Receive();
    using var b := await ch.Receive();
    using var c := await ch.Receive();
    Console.WriteLine(a.Id);
    Console.WriteLine(b.Id);
    Console.WriteLine(c.Id);
  end Main;
end Program;
