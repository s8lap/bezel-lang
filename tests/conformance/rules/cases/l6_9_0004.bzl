//! case: l6_9_0004
//! doc: stdlib
//! section: 6.9
//! title: after the last sender closes, drained receives raise ChannelClosedException
//! rule: Closing. Disposing the last sender closes the channel; buffered
//!   items remain receivable, and after the drain Receive raises
//!   ChannelClosedException.
//! cites: 6.9, 30.15
//! expect: run
//! output: 1
//!   2
//!   closed
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
    snd.Dispose();
    using var a := await ch.Receive();
    using var b := await ch.Receive();
    Console.WriteLine(a.Id);
    Console.WriteLine(b.Id);
    try
      using var c := await ch.Receive();
    catch
      ChannelClosedException => Console.WriteLine("closed"),
    end try;
  end Main;
end Program;
