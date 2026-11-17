//! case: s28_2_4814
//! doc: spec
//! section: 28.2
//! title: a repeated field disposal of a channel and a completion source in
//!   an IDisposable slot disposes once
//! rule: `Dispose()` on any class instance is idempotent: the first call
//!   performs the disposal; subsequent calls are no-ops.
//! cites: 28.2, 28.11
//! expect: run
//! output: RD1
//!   RD2
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

public class Holder
  public owned F: IDisposable? := null;

  public constructor(owned d: IDisposable)
    this.F := d;
  end constructor;

  public method Kill()
    this.F?.Dispose();
    this.F?.Dispose();
  end Kill;
end Holder;

public class Program
  public static method Main()
    var ch := new Channel<Res>();
    using var tx := ch.NewSender();
    tx.Send(new Res(1));
    using var h1 := new Holder(move ch);
    h1.Kill();
    var o := new OwningTaskCompletionSource<Res>();
    o.SetResult(new Res(2));
    using var h2 := new Holder(move o);
    h2.Kill();
    Console.WriteLine("end");
  end Main;
end Program;
