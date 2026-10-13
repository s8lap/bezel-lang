//! case: s29_7_0006
//! doc: spec
//! section: 29.7
//! title: an advance awaited after its enumerator was disposed mid-flight settles false after the cleanups
//! rule: the frame's undischarged `owned` and move-only parameters after them
//!   (§29.8); the machine is completed, and the advance's `Task<bool>` settles
//!   with `false`, or faults with the exception a cleanup action raised ... A
//!   consumer that awaits the advance after disposing the enumerator sees
//!   `false` (or the cleanup's fault), never a value and never a fault of the
//!   body's
//! cites: 29.7, 30.7
//! expect: run
//! output: closed
//!   False
//!   done
//! issue: 5900
module Conf;

public class Res
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Res;

public class Demo
  public static async method Gen(): owned IAsyncEnumerator<int>
    using var r := new Res();
    await Task.Delay(50);
    yield return 1;
  end Gen;

  public static async method Main(): Task
    await using var en := Gen();
    using var adv := en.MoveNextAsync();
    await en.DisposeAsync();
    Console.WriteLine(await adv);
    Console.WriteLine("done");
  end Main;
end Demo;
