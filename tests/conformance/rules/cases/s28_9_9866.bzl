//! case: s28_9_9866
//! doc: spec
//! section: 28.9
//! title: An async override of an abstract Task member reads a closure after the task is returned
//! rule: Function-typed fields, parameters, and locals are not subject to `owned` annotations and carry no disposal obligation
//! cites: 28.9, 28.14, 30.1, 12.6
//! expect: run
//! stdout: 16800
//! notes: Harvested from the #9309 review (p10_async_override). An abstract member declared with a
//!   `Task<int>` return (12.6: an abstract method whose implementations are asynchronous declares
//!   a Task return without `async`), overridden by an `async` method that suspends and then reads
//!   the callback twice. The caller returns the pending task, so the task outlives the closure
//!   local that was passed in. Trace: 42 + 42 = 84 per round, 200 rounds = 16800.
module Conf;

public abstract class Fl
  public abstract method M(q: () -> int): Task<int>;
end Fl;

public class Sub : Fl
  public async override method M(q: () -> int): Task<int>
    await Task.Delay(1);
    return q() + q();
  end M;
end Sub;

public class Program
  public static method Make(fl: Fl): Task<int>
    var c := 42;
    var g := lambda() => c;
    return fl.M(g);
  end Make;

  public static async method Main(): Task<int>
    var t := 0;
    var fl: Fl := new Sub();
    for var j in 0 ..< 200 do
      t := t + await Make(fl);
    end for;
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
