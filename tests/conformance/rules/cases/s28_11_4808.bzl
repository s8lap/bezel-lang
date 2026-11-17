//! case: s28_11_4808
//! doc: spec
//! section: 28.11
//! title: an OwningArray of IDisposable channel handles disposes each element
//!   once
//! rule: the owning collection's cascade disposes every element
//! cites: 28.11, 30.15
//! expect: run
//! output: RD1
//!   True
//!   5
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
    var ch := new Channel<Res>();
    var s1 := ch.NewSender();
    var s2 := ch.NewSender();
    s1.Send(new Res(1));
    var ch2 := new Channel<int>();
    var s3 := ch2.NewSender();
    s3.Send(5);
    var t := new TaskCompletionSource<int>();
    var a := new OwningArray<IDisposable>();
    a.Add(move s1);
    a.Add(move ch);
    a.Add(move s3);
    a.Add(move t);
    a.Add(move s2);
    a.Dispose();
    Console.WriteLine(ch2.IsClosed);
    Console.WriteLine(await ch2.Receive());
    ch2.Dispose();
  end Main;
end Program;
