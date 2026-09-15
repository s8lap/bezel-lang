//! case: l6_9_0012
//! doc: stdlib
//! section: 6.9
//! title: Send on a closed channel raises ChannelClosedException
//! rule: Send(owned item: T); ... ChannelClosedException if closed.
//! cites: 6.9, 30.15
//! expect: run
//! stdout: closed
module Conf;

import System.Threading;

public sealed class Job
  public Id: int;
  public constructor(id: int) Id := id; end constructor;
end Job;

public class Program
  public static method Main()
    using var ch := new Channel<Job>();
    using var snd := ch.NewSender();
    ch.Close();
    try
      snd.Send(new Job(1));
    catch
      ChannelClosedException => Console.WriteLine("closed"),
    end try;
  end Main;
end Program;
