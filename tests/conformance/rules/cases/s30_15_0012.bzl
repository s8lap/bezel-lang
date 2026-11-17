//! case: s30_15_0012
//! doc: spec
//! section: 30.15
//! title: a channel in an owned IDisposable slot disposes undelivered items
//!   in reverse insertion order
//! rule: Disposing the `Channel<T>` itself closes it immediately and disposes
//!   every undelivered class-typed item in reverse insertion order
//! cites: 30.15, 28.11
//! expect: run
//! output: RD2
//!   RD1
//!   end
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
    var ch := new Channel<Res>();
    using var tx := ch.NewSender();
    tx.Send(new Res(1));
    tx.Send(new Res(2));
    do
      using var s := new Slot(move ch);
    end;
    Console.WriteLine("end");
  end Main;
end Program;
