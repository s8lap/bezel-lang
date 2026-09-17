//! case: l6_9_0008
//! doc: stdlib
//! section: 6.9
//! title: ReceiveAll completes normally on drained-and-closed
//! rule: ReceiveAll() ... suspends while empty-and-open, and completes the
//!   enumeration normally on drained-and-closed — ChannelClosedException
//!   stops being control flow for the normal exit.
//! cites: 6.9, 30.15
//! expect: run
//! stdout: 6
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<Job>();
    var snd := ch.NewSender();
    snd.Send(new Job(1));
    snd.Send(new Job(2));
    snd.Send(new Job(3));
    snd.Dispose();
    var total := 0;
    await for var j in ch.ReceiveAll() do
      using var jj := move j;
      total := total + jj.Id;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
