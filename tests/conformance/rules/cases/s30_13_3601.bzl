//! case: s30_13_3601
//! doc: spec
//! section: 30.13
//! title: a startup-thread Result read on a main-domain task waits by releasing the claim
//! rule: Polling is not waiting: a startup thread that waits by reading `IsCompleted` in a loop, on a task
//!   whose completion waits on a main-domain continuation, never releases the claim … wait with
//!   `Wait()`/`Result` instead.
//! cites: 30.13, 30.3
//! expect: run
//! stdout: r=8
//! issue: 9379
//! notes: The issue's program with the polling loop replaced by the remedy. The polling form is not a
//!   case: it never ends, by design.
module Conf;

public class C
  public static async method Twice(u: int): Task<int>
    await Task.Delay(5);
    return u * 2;
  end Twice;
end C;

public class D
  public static method Main(): int
    using var t := C.Twice(4);
    Console.WriteLine("r=" + t.Result.ToString());
    return 0;
  end Main;
end D;
