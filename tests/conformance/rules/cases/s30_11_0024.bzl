//! case: s30_11_0024
//! doc: spec
//! section: 30.11
//! title: moving a completion source into a slot while a borrow of its Task
//!   is used afterwards is rejected
//! rule: A read of the property is a borrow of the task, sourced at the
//!   source object's location
//! cites: 30.11, 28.3
//! expect: reject
//! diagnostic: the borrow held by
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
    var tcs := new TaskCompletionSource<int>();
    tcs.SetResult(3);
    var t := tcs.Task;
    var s := new Slot(move tcs);
    Console.WriteLine(await t);
  end Main;
end Program;
