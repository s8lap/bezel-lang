//! case: s30_15_0011
//! doc: spec
//! section: 30.15
//! title: a Send faulting on a full bounded channel disposes its item before the exception surfaces
//! rule: A send that faults disposes its class-typed item before the exception
//!   surfaces: `ChannelFullException` from the synchronous `Send` on a full
//!   bounded channel, and `ChannelClosedException` from `Send` or `SendAsync`
//!   on a closed one, all leave the item disposed — a channel never retains an
//!   item it did not accept.
//! cites: 30.15
//! expect: run
//! output: dispose 2
//!   full
//!   1
//!   dispose 1
//! notes: l6_9_0007 pins the ChannelFullException itself; this pins the
//!   disposal order and that the accepted item stays buffered.
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
    using var ch := new Channel<Job>(1);
    using var tx := ch.NewSender();
    tx.Send(new Job(1));
    try
      tx.Send(new Job(2));
    catch
      ChannelFullException => Console.WriteLine("full"),
    end try;
    Console.WriteLine(ch.Count);
  end Main;
end Program;
