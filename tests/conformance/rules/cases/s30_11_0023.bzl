//! case: s30_11_0023
//! doc: spec
//! section: 30.11
//! title: an owning completion source dropped through an IDisposable slot
//!   disposes its unawaited result
//! rule: A task **never awaited** by the time the source is disposed is
//!   disposed with the source
//! cites: 30.11, 28.11
//! expect: run
//! output: RD1
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
    var o := new OwningTaskCompletionSource<Res>();
    o.SetResult(new Res(1));
    do
      using var s := new Slot(move o);
    end;
    Console.WriteLine("end");
  end Main;
end Program;
