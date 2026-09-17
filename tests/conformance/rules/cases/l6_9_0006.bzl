//! case: l6_9_0006
//! doc: stdlib
//! section: 6.9
//! title: TryReceive returns false for both empty-open and drained-closed; IsClosed distinguishes
//! rule: TryReceive returns false for both empty-open and drained-closed;
//!   consult IsClosed to distinguish.
//! cites: 6.9, 30.15
//! expect: run
//! output: False
//!   False
//!   False
//!   True
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
    Console.WriteLine(ch.TryReceive(out _));
    Console.WriteLine(ch.IsClosed);
    snd.Dispose();
    Console.WriteLine(ch.TryReceive(out _));
    Console.WriteLine(ch.IsClosed);
  end Main;
end Program;
