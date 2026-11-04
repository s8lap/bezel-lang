//! case: s30_15_0006
//! doc: spec
//! section: 30.15
//! title: a Send after Close() disposes the offered item before ChannelClosedException surfaces
//! rule: **`Close()`** puts the channel in the same closed state — buffered
//!   items remain receivable, subsequent sends fault with
//!   `ChannelClosedException` (disposing the offered item, the rule)
//! cites: 30.15
//! expect: run
//! output: dispose 1
//!   closed
//! notes: the bounded-channels paragraph states the order — a send that faults
//!   disposes its class-typed item before the exception surfaces.
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;

  destructor
    Console.WriteLine($"dispose {Id}");
  end destructor;
end Job;

public class Program
  public static method Main()
    using var ch := new Channel<Job>();
    using var tx := ch.NewSender();
    ch.Close();
    try
      tx.Send(new Job(1));
    catch
      ChannelClosedException => Console.WriteLine("closed"),
    end try;
  end Main;
end Program;
