//! case: l6_9_0014
//! doc: stdlib
//! section: 6.9
//! title: Send on a closed channel raises a failure an IFailure arm takes
//! rule: The failure types this document declares are … `ChannelClosedException`
//!   and `ChannelFullException` (§6.9) … with the built-in `FormatException`
//!   they are the library's whole failure band.
//! cites: 6.9, 1
//! expect: run
//! stdout: True
//! issue: 8414
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
      var f: IFailure => Console.WriteLine(f is ChannelClosedException)
    end;
  end Main;
end Program;
