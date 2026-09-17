//! case: l6_9_0005
//! doc: stdlib
//! section: 6.9
//! title: Close() from the receiving end is idempotent
//! rule: Close() is idempotent: on an already-closed channel it is a
//!   no-op, and later sender disposals remain no-ops with respect to the
//!   closed state.
//! cites: 6.9, 30.15
//! expect: run
//! output: 1
//!   True
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
    ch.Close();
    ch.Close();
    using var a := await ch.Receive();
    Console.WriteLine(a.Id);
    Console.WriteLine(ch.IsClosed);
  end Main;
end Program;
