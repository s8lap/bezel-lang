//! case: s30_15_0005
//! doc: spec
//! section: 30.15
//! title: the channel closes only when the last of several minted senders is disposed
//! rule: **`NewSender(): owned ChannelSender<T>`** mints a sender endpoint; any
//!   number may be minted, and each is an independently owned, sendable object.
//!   … **Closing.** Disposing the last `ChannelSender<T>` *closes* the channel
//! cites: 30.15
//! expect: run
//! output: False
//!   False
//!   True
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var ch := new Channel<int>();
    var a := ch.NewSender();
    var b := ch.NewSender();
    Console.WriteLine(ch.IsClosed);
    a.Dispose();
    Console.WriteLine(ch.IsClosed);
    b.Dispose();
    Console.WriteLine(ch.IsClosed);
  end Main;
end Program;
