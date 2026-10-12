//! case: s30_15_0004
//! doc: spec
//! section: 30.15
//! title: Send transfers ownership into the channel and Receive transfers it out in send order
//! rule: **Ordering.** Items from one sender are received in the order sent ...
//!   **Closing.** Disposing the last `ChannelSender<T>` *closes* the channel:
//!   items already sent remain receivable (the channel drains).
//! cites: 30.15
//! expect: run
//! output: 1
//!   2
//!   True
module Conf;

import System.Threading;

public sealed class Report
  public _id: int;
  public constructor(id: int)
    _id := id;
  end constructor;
end Report;

public class Program
  public static async method Main(): Task
    using var inbox := new Channel<Report>();
    using var tx := inbox.NewSender();
    tx.Send(new Report(1));
    tx.Send(new Report(2));
    tx.Dispose();
    using var a := await inbox.Receive();
    using var b := await inbox.Receive();
    Console.WriteLine(a._id);
    Console.WriteLine(b._id);
    Console.WriteLine(inbox.IsClosed);
  end Main;
end Program;
