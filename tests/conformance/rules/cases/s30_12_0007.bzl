//! case: s30_12_0007
//! doc: spec
//! section: 30.12
//! title: an immediately-available Acquire in a non-async helper reached from a continuation proceeds
//! rule: where static rejection cannot see the call (a non-`async` helper
//!   reached from a continuation), the runtime raises
//!   `InvalidOperationException` when the acquisition would actually park a
//!   thread inside a domain continuation — an immediately-available
//!   acquisition, and any acquisition on the startup thread or an FFI foreign
//!   thread (both outside any domain), proceeds as before.
//! cites: 30.12, 30.13
//! expect: run
//! output: locked
//!   done
//! notes: companion of s30_12_0008, where the same helper would park and raises.
module Conf;

import System.Threading;

public class Program
  public static method Critical(m: Mutex)
    using var g := m.Acquire();
    Console.WriteLine("locked");
  end Critical;

  public static async method Main(): Task
    using var m := new Mutex();
    await Task.Delay(1);
    Critical(m);
    Console.WriteLine("done");
  end Main;
end Program;
