//! case: s29_7_0005
//! doc: spec
//! section: 29.7
//! title: disposing an async enumerator mid-advance neither waits nor raises and defers the frame's cleanups
//! rule: Such a disposal neither waits nor raises: it begins the enumerator's
//!   logical disposal (§28.12) and returns at once (`DisposeAsync()` with a
//!   completed task), and the frame's disposal is **deferred to the pending
//!   advance's settlement** ... The cleanup actions active at that suspension
//!   point then run as on an exit edge of the body
//! cites: 29.7, 30.7, 30.5
//! expect: run
//! output: disposed
//!   closed
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
    Console.WriteLine("disposed");
    _ := await adv;
    Console.WriteLine("done");
  end Main;
end Demo;
