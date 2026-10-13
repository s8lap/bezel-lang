//! case: s28_3_0130
//! doc: spec
//! section: 28.3
//! title: in an async body a raising return-edge cleanup faults the task and disposes the result once
//! rule: In an `async` body the slot is the frame's: the task records its
//!   result at the body's normal completion only (§30.1), and a raise from a
//!   cleanup on the return edge faults the task and disposes the slot's value
//!   in the frame's unwind — a faulted task has no owned result (§30.5).
//! cites: 28.3, 30.1, 30.5
//! expect: run
//! output: closed 1
//!   caught
//! issue: 5809
module Conf;

public class Res
  public property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  destructor
    Console.WriteLine($"closed {N}");
  end destructor;
end Res;

public class Boom
  public constructor()
  end constructor;

  destructor
    throw new InvalidOperationException("boom");
  end destructor;
end Boom;

public class Demo
  public static async method MakeAsync(): OwningTask<Res>
    using var b := new Boom();
    await Task.Delay(1);
    return new Res(1);
  end MakeAsync;

  public static async method Main(): Task
    try
      using var got := await MakeAsync();
      Console.WriteLine($"got {got.N}");
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Demo;
