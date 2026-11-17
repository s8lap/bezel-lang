//! case: s30_15_0013
//! doc: spec
//! section: 30.15
//! title: the last sender in an owned IDisposable slot closes the channel and
//!   it drains
//! rule: Disposing the last `ChannelSender<T>` *closes* the channel: items
//!   already sent remain receivable (the channel drains), and after the
//!   drain, receives raise `ChannelClosedException`
//! cites: 30.15, 28.11
//! expect: run
//! output: 7
//!   closed
module Conf;

import System.Threading;

public sealed class Res
  private n: int;

  public constructor(n: int)
    this.n := n;
  end constructor;

  destructor
    Console.WriteLine($"RD{this.n}");
  end destructor;
end Res;

public class Slot
  public owned F: IDisposable? := null;

  public constructor(owned d: IDisposable)
    this.F := d;
  end constructor;
end Slot;

public class Program
  public static async method Main(): Task
    using var ch := new Channel<int>();
    var tx := ch.NewSender();
    tx.Send(7);
    do
      using var s := new Slot(move tx);
    end;
    Console.WriteLine(await ch.Receive());
    try
      var v := await ch.Receive();
      Console.WriteLine(v);
    catch
      ChannelClosedException => Console.WriteLine("closed"),
    end try;
  end Main;
end Program;
