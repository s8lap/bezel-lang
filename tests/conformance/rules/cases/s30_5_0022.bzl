//! case: s30_5_0022
//! doc: spec
//! section: 30.5
//! title: a Pattern A kind-2 chained slot disposes the source task transitively, exactly once
//! rule: 2. **`return await` over an owned Pattern A source — the chained
//!   slot** ... ownership of the **source task** transfers into the outer
//!   task's state ... and the outer destructor disposes the chained source
//!   task, never the borrowed result directly. "Disposing the task disposes
//!   the result" thereby holds *transitively*.
//! cites: 30.5
//! expect: run
//! output: got
//!   res disposed
module Conf;

public class Res
  destructor
    Console.WriteLine("res disposed");
  end destructor;
end Res;

public class Inner
  public static async method MakeAsync(): Task<Res>
    await Task.Delay(0);
    return new Res();
  end MakeAsync;
end Inner;

public class Outer
  public static async method RelayAsync(): Task<Res>
    using var src := Inner.MakeAsync();
    return await src;
  end RelayAsync;
end Outer;

public class Program
  public static async method Main(): Task
    using var t := Outer.RelayAsync();
    var r := await t;
    Console.WriteLine("got");
  end Main;
end Program;
