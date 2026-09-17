//! case: l6_9_0009
//! doc: stdlib
//! section: 6.9
//! title: disposing the channel disposes undelivered items in reverse insertion order
//! rule: Disposing the Channel<T> closes it immediately and disposes every
//!   undelivered class-typed item in reverse insertion order; subsequent
//!   Send raises ChannelClosedException.
//! cites: 6.9, 30.15, 28.11
//! expect: run
//! output: dispose 3
//!   dispose 2
//!   dispose 1
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
    var ch := new Channel<Job>();
    using var snd := ch.NewSender();
    snd.Send(new Job(1));
    snd.Send(new Job(2));
    snd.Send(new Job(3));
    ch.Dispose();
  end Main;
end Program;
