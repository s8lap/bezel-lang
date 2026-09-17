//! case: l6_9_0007
//! doc: stdlib
//! section: 6.9
//! title: synchronous Send on a full bounded channel raises ChannelFullException
//! rule: on a FULL bounded channel raises ChannelFullException (v0.174);
//!   ChannelClosedException if closed
//! cites: 6.9, 30.15
//! expect: run
//! stdout: full
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public class Program
  public static method Main()
    using var ch := new Channel<Job>(1);
    using var snd := ch.NewSender();
    snd.Send(new Job(1));
    try
      snd.Send(new Job(2));
    catch
      ChannelFullException => Console.WriteLine("full"),
    end try;
  end Main;
end Program;
