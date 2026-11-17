//! case: s30_15_0014
//! doc: spec
//! section: 30.15
//! title: a send after the channel is disposed through an owned IDisposable
//!   slot raises ChannelClosedException
//! rule: Disposing the `Channel<T>` itself closes it immediately
//! cites: 30.15, 28.11
//! expect: run
//! stdout: CCE
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
  public static method Main()
    var ch := new Channel<int>();
    using var tx := ch.NewSender();
    do
      using var s := new Slot(move ch);
    end;
    try
      tx.Send(1);
    catch
      ChannelClosedException => Console.WriteLine("CCE"),
    end try;
  end Main;
end Program;
