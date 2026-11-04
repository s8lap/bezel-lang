//! case: s30_15_0007
//! doc: spec
//! section: 30.15
//! title: disposing the Channel itself closes it immediately for a surviving sender
//! rule: Disposing the `Channel<T>` itself closes it immediately and disposes
//!   every undelivered class-typed item in reverse insertion order (the owned
//!   column of §28.11); subsequent sends raise `ChannelClosedException`.
//! cites: 30.15
//! expect: run
//! stdout: closed
//! notes: l6_9_0009 pins the reverse-order disposal of undelivered items; this
//!   pins the immediate close as seen from a sender that outlives the channel.
module Conf;

import System.Threading;

public class Program
  public static method Main()
    var ch := new Channel<int>();
    using var tx := ch.NewSender();
    tx.Send(1);
    ch.Dispose();
    try
      tx.Send(2);
    catch
      ChannelClosedException => Console.WriteLine("closed"),
    end try;
  end Main;
end Program;
